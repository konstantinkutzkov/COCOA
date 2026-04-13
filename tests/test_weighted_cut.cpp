// Test: use distance-weighted node capacities to find balanced cuts
// Nodes near s or t get high capacity, nodes far from both get cap=1
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <queue>
#include <string>
#include "../src/dinic.h"
#include "../src/separator.h"

int main(int argc, char *argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <cnf_file> <s_var> <t_var>" << std::endl;
        return 1;
    }

    int s_var = atoi(argv[2]);
    int t_var = atoi(argv[3]);

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

    // Build FormulaInfo
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

    // BFS distance from s and t in the incidence graph
    auto bfs_dist = [&](int start_var_idx) -> std::vector<int> {
        std::vector<int> dist_var(n_vars, -1);
        std::vector<int> dist_cls(n_cls, -1);
        std::queue<std::pair<bool, int>> q;
        dist_var[start_var_idx] = 0;
        q.push({false, start_var_idx});
        while (!q.empty()) {
            auto [is_cls, idx] = q.front(); q.pop();
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
        // Combine into node distances
        std::vector<int> dist(n_nodes);
        for (int i = 0; i < n_vars; i++) dist[i] = dist_var[i];
        for (int i = 0; i < n_cls; i++) dist[n_vars + i] = dist_cls[i];
        return dist;
    };

    int s_idx = info.var_id_to_idx.at(s_var);
    int t_idx = info.var_id_to_idx.at(t_var);

    auto dist_s = bfs_dist(s_idx);
    auto dist_t = bfs_dist(t_idx);

    // Build Dinic's graph with distance-weighted capacities
    auto n_in = [](int i) { return 2 * i; };
    auto n_out = [](int i) { return 2 * i + 1; };

    Dinic dinic(2 * n_nodes);

    for (int i = 0; i < n_nodes; i++) {
        int cap;
        if (i == s_idx || i == (n_vars + t_idx)) {
            cap = INF_CAP;  // terminals
        } else {
            // Distance-based capacity: nodes close to s or t are expensive
            int ds = dist_s[i];
            int dt = dist_t[i];
            int min_dist = std::min(ds, dt);
            // Nodes within distance 2 of either terminal: infinite cap
            // Others: cap=1
            if (min_dist <= 3)
                cap = INF_CAP;
            else
                cap = 1;
        }
        dinic.add_edge(n_in(i), n_out(i), cap);
    }

    // Add incidence edges with INF capacity
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

    int flow = dinic.max_flow(source, sink, 200);
    auto reach = dinic.reachable_from(source);

    // Extract separator
    std::vector<CutNode> separator;
    for (int i = 0; i < n_vars; i++) {
        if (reach[n_in(i)] && !reach[n_out(i)]) {
            separator.push_back(CutNode(CutNode::VAR, info.active_vars[i]));
        }
    }
    for (int j = 0; j < n_cls; j++) {
        int node_j = n_vars + j;
        if (reach[n_in(node_j)] && !reach[n_out(node_j)]) {
            separator.push_back(CutNode(CutNode::CLAUSE, info.active_clause_ids[j]));
        }
    }

    std::cout << "Flow: " << flow << std::endl;
    std::cout << "Separator: " << separator.size() << " nodes" << std::endl;

    int nv = 0, nc = 0;
    for (const auto &nd : separator) {
        if (nd.kind == CutNode::VAR) nv++;
        else nc++;
    }
    std::cout << "  Variables: " << nv << "  Clauses: " << nc << std::endl;

    auto sizes = verify_separator(info, separator, 1);
    if (!sizes.empty()) {
        std::cout << "Component sizes:";
        for (int s : sizes) std::cout << " " << s;
        std::cout << std::endl;
    }

    return 0;
}
