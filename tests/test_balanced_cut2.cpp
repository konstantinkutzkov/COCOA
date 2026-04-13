// Brute force: try ALL pairs of variables as terminals,
// run Dinic's, keep the most balanced separator.
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <string>
#include "../src/separator.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <cnf_file> [flow_cap]" << std::endl;
        return 1;
    }

    int flow_cap = argc > 2 ? atoi(argv[2]) : 50;

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

    // Try many terminal pairs
    int best_balance = 0;
    int best_sep_size = 0;
    std::vector<CutNode> best_separator;
    std::vector<int> best_sizes;
    int best_s = 0, best_t = 0;
    int total_tried = 0;

    // Sample terminal pairs (not all n^2, but enough)
    for (int si = 1; si <= n_vars; si += 2) {
        for (int ti = si + 1; ti <= n_vars; ti += 3) {
            CutNode s(CutNode::VAR, si);
            CutNode t(CutNode::VAR, ti);

            MinCutResult res = compute_mincut_separator(info, s, t, flow_cap);
            total_tried++;

            if (res.separator.empty()) continue;

            auto sizes = verify_separator(info, res.separator, 1);
            if (sizes.size() < 2) continue;

            int balance = sizes[1]; // second largest component
            if (balance > best_balance ||
                (balance == best_balance && (int)res.separator.size() < best_sep_size)) {
                best_balance = balance;
                best_sep_size = res.separator.size();
                best_separator = res.separator;
                best_sizes = sizes;
                best_s = si;
                best_t = ti;
            }
        }
    }

    std::cout << "Tried " << total_tried << " terminal pairs" << std::endl;

    if (best_balance > 0) {
        int n_var = 0, n_cls = 0;
        for (const auto &nd : best_separator) {
            if (nd.kind == CutNode::VAR) n_var++;
            else n_cls++;
        }
        std::cout << "Best separator: size=" << best_sep_size
                  << " (" << n_var << "V+" << n_cls << "C)"
                  << " balance=" << best_balance
                  << " s=V" << best_s << " t=V" << best_t
                  << std::endl;
        std::cout << "Component sizes:";
        for (int s : best_sizes) std::cout << " " << s;
        std::cout << std::endl;
    } else {
        std::cout << "No useful separator found" << std::endl;
    }

    return 0;
}
