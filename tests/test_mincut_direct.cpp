// Test compute_mincut_separator directly with specific terminals
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include "../src/separator.h"

int main(int argc, char *argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <cnf_file> <s_var> <t_var> [flow_cap]" << std::endl;
        return 1;
    }

    int s_var = atoi(argv[2]);
    int t_var = atoi(argv[3]);
    int flow_cap = argc > 4 ? atoi(argv[4]) : 30;

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

    CutNode s(CutNode::VAR, s_var);
    CutNode t(CutNode::VAR, t_var);

    std::cout << "Computing mincut between var " << s_var << " and var " << t_var
              << " with flow_cap=" << flow_cap << std::endl;

    MinCutResult res = compute_mincut_separator(info, s, t, flow_cap);

    std::cout << "Flow value: " << res.flow_value << std::endl;
    std::cout << "Separator size: " << res.separator.size() << std::endl;

    int n_var = 0, n_cls = 0;
    for (const auto &nd : res.separator) {
        if (nd.kind == CutNode::VAR) n_var++;
        else n_cls++;
    }
    std::cout << "  Variables: " << n_var << "  Clauses: " << n_cls << std::endl;

    if (!res.separator.empty()) {
        auto sizes = verify_separator(info, res.separator, 1);
        if (!sizes.empty()) {
            std::cout << "Verified. Component sizes:";
            for (int s : sizes) std::cout << " " << s;
            std::cout << std::endl;
        } else {
            std::cout << "Verification FAILED" << std::endl;
        }
    }

    return 0;
}
