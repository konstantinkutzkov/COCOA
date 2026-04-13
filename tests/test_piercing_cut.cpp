// Iterative piercing balanced separator on the incidence graph.
// Inspired by flowcutter: grow source/sink sets by piercing boundary nodes,
// track best balanced cut.
//
// Key advantage over flowcutter on primal graph: clause nodes are first-class
// citizens with capacity 1, so a single clause removal can disconnect many
// variables.

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <queue>
#include <set>
#include <string>
#include <algorithm>
#include "../src/dinic.h"
#include "../src/separator.h"

struct BalancedResult {
    std::vector<CutNode> separator;
    int n_var_sep = 0, n_cls_sep = 0;
    int side_a = 0, side_b = 0;
    int flow = 0;
    int iteration = -1;
};

BalancedResult find_balanced_separator_piercing(
    const FormulaInfo &info,
    int s_var, int t_var,
    int max_iterations = 100)
{
    int n_vars = info.active_vars.size();
    int n_cls = info.active_clause_ids.size();
    int n_nodes = n_vars + n_cls;

    int s_idx = info.var_id_to_idx.at(s_var);
    int t_idx = info.var_id_to_idx.at(t_var);

    // Track which nodes are absorbed into source/sink side
    std::vector<bool> source_side(n_nodes, false);
    std::vector<bool> sink_side(n_nodes, false);
    source_side[s_idx] = true;
    sink_side[t_idx] = true;

    auto n_in = [](int i) { return 2 * i; };
    auto n_out = [](int i) { return 2 * i + 1; };

    BalancedResult best;
    best.side_b = 0;

    for (int iter = 0; iter < max_iterations; iter++) {
        // Build Dinic's graph with current source/sink sets
        Dinic dinic(2 * n_nodes);

        for (int i = 0; i < n_nodes; i++) {
            int cap;
            if (source_side[i] || sink_side[i])
                cap = INF_CAP;  // absorbed nodes can't be cut
            else
                cap = 1;
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

        int flow = dinic.max_flow(source, sink, n_nodes);
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

        // Compute component sizes (variables only)
        int source_var_count = 0, sink_var_count = 0, sep_var_count = 0;
        std::set<int> sep_indices;
        for (int i = 0; i < n_vars; i++) {
            if (reach[n_in(i)] && !reach[n_out(i)])
                sep_indices.insert(i);
        }

        for (int i = 0; i < n_vars; i++) {
            if (sep_indices.count(i)) {
                sep_var_count++;
            } else if (reach[n_out(i)]) {
                source_var_count++;
            } else {
                sink_var_count++;
            }
        }

        int small_side = std::min(source_var_count, sink_var_count);
        int big_side = std::max(source_var_count, sink_var_count);

        int nv = 0, nc = 0;
        for (const auto &nd : separator)
            if (nd.kind == CutNode::VAR) nv++; else nc++;

        // Check if this is the best balanced cut so far
        if (small_side > best.side_b ||
            (small_side == best.side_b && (int)separator.size() < best.n_var_sep + best.n_cls_sep)) {
            best.separator = separator;
            best.n_var_sep = nv;
            best.n_cls_sep = nc;
            best.side_a = big_side;
            best.side_b = small_side;
            best.flow = flow;
            best.iteration = iter;
        }

        // Stop if balanced enough
        if (small_side >= n_vars / 4)
            break;

        // Determine which side is smaller and should be grown
        bool grow_source = (source_var_count <= sink_var_count);

        // Find a pierce node: a node in the separator or on the cut boundary
        // that we absorb into the smaller side
        // Pick the one that is most "useful" — for now, just pick the first
        // separator node (preferring clause nodes as they're cheaper to branch on)
        int pierce_node = -1;

        // Absorb the small side + separator into the SINK (growing it).
        // The source stays small. Next iteration, Dinic's must find a
        // cut that separates the source from the now-larger sink.
        //
        // Key insight: source_var_count is the # of vars reachable from s
        // in the residual graph. If source is the big side, we want to
        // grow the SINK by absorbing the small side + separator into it.
        // If source is the small side, grow it by absorbing from the other side.

        // Determine which side in the residual graph is small
        // source side = reach[n_out(i)], sink side = !reach[n_in(i)]
        // separator = reach[n_in(i)] && !reach[n_out(i)]

        // Count actual sides
        int n_source = 0, n_sink = 0;
        for (int i = 0; i < n_nodes; i++) {
            if (reach[n_out(i)]) n_source++;
            else if (!reach[n_in(i)]) n_sink++;
        }

        // Determine which terminal set is smaller (by absorbed node count)
        int src_absorbed = 0, snk_absorbed = 0;
        for (int i = 0; i < n_nodes; i++) {
            if (source_side[i]) src_absorbed++;
            if (sink_side[i]) snk_absorbed++;
        }

        // Grow the SMALLER terminal set by absorbing the separator nodes
        // and the small component into it
        bool grow_src = (src_absorbed <= snk_absorbed);

        // Absorb separator + small side into the growing side
        for (int i = 0; i < n_nodes; i++) {
            // Separator nodes
            if (reach[n_in(i)] && !reach[n_out(i)]) {
                if (grow_src) source_side[i] = true;
                else sink_side[i] = true;
            }
            // Small component nodes (non-separator, non-terminal)
            if (grow_src && !reach[n_in(i)] && !source_side[i])
                source_side[i] = true;  // absorb sink-side into source
            if (!grow_src && reach[n_out(i)] && !sink_side[i])
                sink_side[i] = true;  // absorb source-side into sink
        }

        pierce_node = 0;  // prevent break

        if (pierce_node < 0)
            break;
    }

    return best;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <cnf_file> [tries]" << std::endl;
        return 1;
    }

    int tries = argc > 2 ? atoi(argv[2]) : 10;

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

    std::cout << "Variables: " << n_vars << ", Clauses: " << clauses.size() << std::endl;

    BalancedResult overall_best;
    overall_best.side_b = 0;

    for (int i = 0; i < tries; i++) {
        auto terminals = pick_terminals_two_sweep(info, i);
        int s = terminals.first.id;
        int t = terminals.second.id;

        auto result = find_balanced_separator_piercing(info, s, t);

        std::cout << "  Try " << i << ": s=V" << s << " t=V" << t
                  << " iter=" << result.iteration
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
              << " iter=" << overall_best.iteration << std::endl;

    return 0;
}
