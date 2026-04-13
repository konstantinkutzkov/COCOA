// Test iterative balanced separator finding
// After each min-cut, merge the small side into the source terminal set
// and re-run until we get a balanced cut or exhaust the budget.

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <string>
#include "../src/separator.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <cnf_file> [max_iters] [flow_cap]" << std::endl;
        return 1;
    }

    int max_iters = argc > 2 ? atoi(argv[2]) : 20;
    int flow_cap = argc > 3 ? atoi(argv[3]) : 50;

    // Parse CNF
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

    std::cout << "Variables: " << n_vars << ", Clauses: " << clauses.size() << std::endl;

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

    // Pick initial terminals via two-sweep BFS
    auto terminals = pick_terminals_two_sweep(info, 42);
    CutNode s = terminals.first;
    CutNode t = terminals.second;
    std::cout << "Initial terminals: s=V" << s.id << " t=V" << t.id << std::endl;

    // Iterative balanced cut finding
    // Maintain sets of nodes that must be on the source/sink side
    // by giving them infinite capacity (i.e., using them as multi-terminals)
    std::set<unsigned> source_vars, sink_vars;
    source_vars.insert(s.id);
    sink_vars.insert(t.id);

    for (int iter = 0; iter < max_iters; iter++) {
        // Run min-cut with current source terminal
        MinCutResult res = compute_mincut_separator(info, s, t, flow_cap);

        std::cout << "Iter " << iter << ": flow=" << res.flow_value
                  << " sep=" << res.separator.size();

        if (res.separator.empty()) {
            std::cout << " (empty separator, stopping)" << std::endl;
            break;
        }

        // Verify and get component sizes
        auto sizes = verify_separator(info, res.separator, 1);
        if (sizes.empty()) {
            std::cout << " (verification failed)" << std::endl;
            break;
        }

        int n_var_sep = 0, n_cls_sep = 0;
        for (const auto &nd : res.separator) {
            if (nd.kind == CutNode::VAR) n_var_sep++;
            else n_cls_sep++;
        }

        std::cout << " (" << n_var_sep << "V+" << n_cls_sep << "C)"
                  << " comps=";
        for (int sz : sizes) std::cout << sz << " ";

        // Check balance
        if (sizes.size() >= 2 && sizes[1] >= (int)(n_vars / 4)) {
            std::cout << " BALANCED!" << std::endl;
            std::cout << "\nFinal separator (" << res.separator.size() << " nodes):" << std::endl;
            for (const auto &nd : res.separator) {
                std::cout << "  " << (nd.kind == CutNode::VAR ? "V" : "C") << nd.id << std::endl;
            }
            break;
        }

        std::cout << std::endl;

        // Merge small side into source and re-run
        // Find which component contains source vs sink
        std::set<CutNode> removed(res.separator.begin(), res.separator.end());
        auto comps = components_after_removing(info, removed);

        // The separator has 3 clauses. The "small side" might have 0 vars
        // (just isolated clauses). We need a different approach:
        // Instead of merging components, we force the cut to be deeper
        // by making separator nodes part of the source and increasing flow.

        // Absorb all separator variable nodes into source set
        for (const auto &nd : res.separator) {
            if (nd.kind == CutNode::VAR)
                source_vars.insert(nd.id);
        }

        // Also absorb all variables from small components
        for (size_t ci = 0; ci < comps.size(); ci++) {
            int comp_var_count = component_variable_count(comps[ci]);
            // Check if this component contains any source var
            bool is_source_comp = false;
            for (const auto &nd : comps[ci]) {
                if (nd.kind == CutNode::VAR && source_vars.count(nd.id)) {
                    is_source_comp = true;
                    break;
                }
            }
            if (!is_source_comp && comp_var_count < (int)(n_vars / 4)) {
                for (const auto &nd : comps[ci]) {
                    if (nd.kind == CutNode::VAR)
                        source_vars.insert(nd.id);
                }
            }
        }

        // The key insight: we need to make source_vars have infinite capacity
        // in Dinic's. Our current API only supports single s,t terminals.
        // Workaround: pick a new s from the expanded set that is far from t.
        // This isn't perfect but approximates multi-terminal source.

        // Pick new s: farthest source_var from t in BFS
        // For now, just pick any var from source set that differs from current s
        bool found_new_s = false;
        for (unsigned v : source_vars) {
            if (v != s.id && !sink_vars.count(v)) {
                s = CutNode(CutNode::VAR, v);
                found_new_s = true;
                break;
            }
        }
        if (!found_new_s) {
            std::cout << "Cannot find new source terminal, stopping" << std::endl;
            break;
        }

        std::cout << "  Source set: " << source_vars.size()
                  << " vars, new s=V" << s.id << std::endl;
    }

    return 0;
}
