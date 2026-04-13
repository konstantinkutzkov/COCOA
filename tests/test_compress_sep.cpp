// Compress a balanced separator by greedily removing redundant nodes.
// After finding a balanced separator, try removing each node and check
// if the separator is still valid. Prefer removing variables over clauses.

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <random>
#include <algorithm>
#include <set>
#include "../src/dinic.h"
#include "../src/separator.h"

// Reuse the combined approach from test_combined_cut
// (simplified: just find one good separator)

struct SepResult {
    std::vector<CutNode> separator;
    int side_a, side_b;
};

SepResult find_initial_separator(
    const FormulaInfo &info, int n_vars, int n_cls,
    int tries, int max_iters, int walks_per_iter)
{
    int n_nodes = n_vars + n_cls;

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

    SepResult best;
    best.side_b = 0;

    for (int tri = 0; tri < tries; tri++) {
        auto terminals = pick_terminals_two_sweep(info, tri);
        int s_idx = info.var_id_to_idx.at(terminals.first.id);
        int t_idx = info.var_id_to_idx.at(terminals.second.id);

        std::vector<bool> source_set(n_nodes, false);
        std::vector<bool> sink_set(n_nodes, false);
        source_set[s_idx] = true;
        sink_set[t_idx] = true;

        std::mt19937 rng(tri * 1000 + 42);

        SepResult try_best;
        try_best.side_b = 0;

        for (int iter = 0; iter < max_iters; iter++) {
            Dinic dinic(2 * n_nodes);

            for (int i = 0; i < n_nodes; i++) {
                int cap = (source_set[i] || sink_set[i]) ? INF_CAP : 1;
                dinic.add_edge(n_in(i), n_out(i), cap);
            }

            for (int ci = 0; ci < n_cls; ci++) {
                int c_node = n_vars + ci;
                for (unsigned vid : info.clause_variables[ci]) {
                    auto it = info.var_id_to_idx.find(vid);
                    if (it == info.var_id_to_idx.end()) continue;
                    dinic.add_edge(n_out(it->second), n_in(c_node), INF_CAP);
                    dinic.add_edge(n_out(c_node), n_in(it->second), INF_CAP);
                }
            }

            int walk_len = 4 + (iter % 4) * 2;
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

            int flow = dinic.max_flow(n_out(s_idx), n_in(t_idx), n_nodes);
            auto reach = dinic.reachable_from(n_out(s_idx));

            std::vector<CutNode> separator;
            int source_vars = 0, sink_vars = 0;
            for (int i = 0; i < n_vars; i++) {
                if (reach[n_in(i)] && !reach[n_out(i)])
                    separator.push_back(CutNode(CutNode::VAR, info.active_vars[i]));
                else if (reach[n_out(i)]) source_vars++;
                else sink_vars++;
            }
            for (int j = 0; j < n_cls; j++) {
                int nj = n_vars + j;
                if (reach[n_in(nj)] && !reach[n_out(nj)])
                    separator.push_back(CutNode(CutNode::CLAUSE, info.active_clause_ids[j]));
            }

            int balance = std::min(source_vars, sink_vars);
            if (balance > try_best.side_b) {
                try_best.separator = separator;
                try_best.side_a = std::max(source_vars, sink_vars);
                try_best.side_b = balance;
            }

            if (balance >= n_vars / 4) break;

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

        if (try_best.side_b > best.side_b) best = try_best;
    }

    return best;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <cnf_file>" << std::endl;
        return 1;
    }

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

    // Step 1: Find initial balanced separator
    auto initial = find_initial_separator(info, n_vars, n_cls, 20, 30, 30);

    int nv = 0, nc = 0;
    for (const auto &nd : initial.separator)
        if (nd.kind == CutNode::VAR) nv++; else nc++;

    std::cout << "\nInitial separator: " << initial.separator.size()
              << " (" << nv << "V+" << nc << "C)"
              << " sides=" << initial.side_a << "/" << initial.side_b
              << std::endl;

    // Step 2: Greedy compression
    // Try removing each node from separator (variables first, then clauses)
    // Keep removal if separator is still valid with min_second >= initial.side_b / 2

    int min_second = std::max(2, initial.side_b / 2);
    std::vector<CutNode> compressed = initial.separator;

    // Sort: try removing variables first (more expensive to branch on)
    std::sort(compressed.begin(), compressed.end(),
        [](const CutNode &a, const CutNode &b) {
            if (a.kind != b.kind) return a.kind == CutNode::VAR;  // vars first
            return a.id < b.id;
        });

    int removed = 0;
    for (size_t i = 0; i < compressed.size(); ) {
        // Try removing node i
        std::vector<CutNode> candidate;
        for (size_t j = 0; j < compressed.size(); j++)
            if (j != i) candidate.push_back(compressed[j]);

        auto sizes = verify_separator(info, candidate, min_second);
        if (!sizes.empty()) {
            // Removal is valid — keep the smaller separator
            compressed = candidate;
            removed++;
            // Don't increment i — the next node is now at position i
        } else {
            i++;
        }
    }

    nv = 0; nc = 0;
    for (const auto &nd : compressed)
        if (nd.kind == CutNode::VAR) nv++; else nc++;

    auto final_sizes = verify_separator(info, compressed, 1);
    int final_a = final_sizes.empty() ? 0 : final_sizes[0];
    int final_b = final_sizes.size() > 1 ? final_sizes[1] : 0;

    std::cout << "Compressed separator: " << compressed.size()
              << " (" << nv << "V+" << nc << "C)"
              << " sides=" << final_a << "/" << final_b
              << " (removed " << removed << " nodes)"
              << std::endl;

    // Step 3: Try re-running Dinic's between the two sides
    // using the compressed separator as a starting point.
    // Fix variables on each side (INF cap) and find a new min-cut
    // through the separator region. This might find a smaller cut
    // through the same gap.

    std::set<unsigned> sep_var_ids, sep_cls_ids;
    for (const auto &nd : compressed) {
        if (nd.kind == CutNode::VAR) sep_var_ids.insert(nd.id);
        else sep_cls_ids.insert(nd.id);
    }

    // Determine which variables are on which side
    std::set<CutNode> removed_set(compressed.begin(), compressed.end());
    auto comps = components_after_removing(info, removed_set);

    if (comps.size() >= 2) {
        // Sort by variable count descending
        std::sort(comps.begin(), comps.end(),
            [](const std::vector<CutNode> &a, const std::vector<CutNode> &b) {
                return component_variable_count(a) > component_variable_count(b);
            });

        std::set<unsigned> side_a_vars, side_b_vars;
        for (const auto &nd : comps[0])
            if (nd.kind == CutNode::VAR) side_a_vars.insert(nd.id);
        for (size_t ci = 1; ci < comps.size(); ci++)
            for (const auto &nd : comps[ci])
                if (nd.kind == CutNode::VAR) side_b_vars.insert(nd.id);

        // Run Dinic's with side_a and side_b as multi-terminal sets
        int n_nodes = n_vars + n_cls;
        auto n_in = [](int i) { return 2 * i; };
        auto n_out = [](int i) { return 2 * i + 1; };

        // Pick representative terminals
        int s_idx = info.var_id_to_idx.at(*side_a_vars.begin());
        int t_idx = info.var_id_to_idx.at(*side_b_vars.begin());

        Dinic dinic(2 * n_nodes);

        for (int i = 0; i < n_vars; i++) {
            unsigned vid = info.active_vars[i];
            int cap;
            if (side_a_vars.count(vid) || side_b_vars.count(vid))
                cap = INF_CAP;  // side nodes can't be cut
            else
                cap = 1;  // separator nodes can be cut
            dinic.add_edge(n_in(i), n_out(i), cap);
        }
        for (int j = 0; j < n_cls; j++) {
            unsigned cid = info.active_clause_ids[j];
            int cap = 1;
            int nj = n_vars + j;
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

        // Give all side_a vars INF cap as source, all side_b as sink
        // Already done above via the cap assignment

        int flow = dinic.max_flow(n_out(s_idx), n_in(t_idx), n_nodes);
        auto reach = dinic.reachable_from(n_out(s_idx));

        std::vector<CutNode> refined_sep;
        int rv = 0, rc = 0;
        for (int i = 0; i < n_vars; i++) {
            if (reach[n_in(i)] && !reach[n_out(i)]) {
                refined_sep.push_back(CutNode(CutNode::VAR, info.active_vars[i]));
                rv++;
            }
        }
        for (int j = 0; j < n_cls; j++) {
            int nj = n_vars + j;
            if (reach[n_in(nj)] && !reach[n_out(nj)]) {
                refined_sep.push_back(CutNode(CutNode::CLAUSE, info.active_clause_ids[j]));
                rc++;
            }
        }

        auto refined_sizes = verify_separator(info, refined_sep, 1);
        int ra = refined_sizes.empty() ? 0 : refined_sizes[0];
        int rb = refined_sizes.size() > 1 ? refined_sizes[1] : 0;

        std::cout << "Refined (Dinic between sides): " << refined_sep.size()
                  << " (" << rv << "V+" << rc << "C)"
                  << " sides=" << ra << "/" << rb
                  << " flow=" << flow << std::endl;
    }

    return 0;
}
