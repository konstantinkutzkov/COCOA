// Standalone test: run find_best_separator on a CNF file
// Build: g++ -std=c++11 -O2 -I src tests/test_separator_on_cnf.cpp src/separator.cpp -o tests/test_sep_cnf

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include "../src/separator.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <cnf_file> [max_sep_size] [tries]" << std::endl;
        return 1;
    }

    int max_sep_size = argc > 2 ? atoi(argv[2]) : 30;
    int tries = argc > 3 ? atoi(argv[3]) : 20;

    // Parse CNF
    std::ifstream fin(argv[1]);
    std::string line;
    int n_vars = 0, n_cls = 0;
    std::vector<std::vector<int>> clauses;

    while (std::getline(fin, line)) {
        if (line.empty() || line[0] == 'c') continue;
        if (line[0] == 'p') {
            sscanf(line.c_str(), "p cnf %d %d", &n_vars, &n_cls);
            continue;
        }
        std::istringstream iss(line);
        std::vector<int> clause;
        int lit;
        while (iss >> lit) {
            if (lit == 0) break;
            clause.push_back(lit);
        }
        if (!clause.empty())
            clauses.push_back(clause);
    }

    std::cout << "Variables: " << n_vars << ", Clauses: " << clauses.size() << std::endl;

    // Build FormulaInfo
    FormulaInfo info;
    for (int v = 1; v <= n_vars; v++)
        info.active_vars.push_back(v);

    for (size_t i = 0; i < clauses.size(); i++) {
        info.active_clause_ids.push_back(i);
        std::vector<unsigned> vars_in_cl;
        for (int lit : clauses[i])
            vars_in_cl.push_back(abs(lit));
        info.clause_variables.push_back(vars_in_cl);
    }
    info.buildIndex();

    // Try different max sizes and min second comp
    for (int min_second : {2, 5}) {
    for (int ms = 5; ms <= max_sep_size; ms += 5) {
        SeparatorCandidate result;
        bool found = find_best_separator(info, result, tries, 0,
            min_second,  // min_second_component_vars
            ms,  // max_separator_size
            2,   // early_stop_size
            ms); // flow_cap_nodes

        if (found) {
            int n_var_nodes = 0, n_cls_nodes = 0;
            for (const auto &nd : result.separator) {
                if (nd.kind == CutNode::VAR) n_var_nodes++;
                else n_cls_nodes++;
            }
            std::cout << "max_size=" << ms
                      << " FOUND separator size=" << result.separator.size()
                      << " (vars=" << n_var_nodes << " cls=" << n_cls_nodes << ")"
                      << " comp_sizes=";
            for (int s : result.component_var_sizes)
                std::cout << s << " ";
            std::cout << " tries_used=" << result.tries_used << std::endl;
            std::cout << " min_second=" << min_second << std::endl;
        } else {
            std::cout << "max_size=" << ms << " min_second=" << min_second << " NOT FOUND" << std::endl;
        }
    }
    }

    return 0;
}
