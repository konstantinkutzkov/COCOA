// Quick test: run METIS_ComputeVertexSeparator on the bipartite incidence
// graph of a CNF formula and print the resulting separator.
//
// Build:
//   g++ -O2 -I/path/to/METIS/include -I/path/to/GKlib/include \
//       test_metis_sep.cpp -L/path/to/METIS/build/libmetis -lmetis \
//       -L/path/to/GKlib/build -lGKlib -lm -o test_metis_sep

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <set>
#include <fstream>
#include <sstream>
#include <string>
#include <metis.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <cnf-file>\n", argv[0]);
        return 1;
    }

    // Parse CNF
    std::ifstream in(argv[1]);
    int n_vars = 0, n_cls = 0;
    std::vector<std::vector<int>> clauses;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == 'c') continue;
        if (line[0] == 'p') {
            sscanf(line.c_str(), "p cnf %d %d", &n_vars, &n_cls);
            continue;
        }
        std::istringstream iss(line);
        std::vector<int> cls;
        int lit;
        while (iss >> lit && lit != 0)
            cls.push_back(abs(lit));
        if (!cls.empty()) clauses.push_back(cls);
    }
    n_cls = clauses.size();

    printf("Parsed: %d vars, %d clauses\n", n_vars, n_cls);

    // Build bipartite incidence graph in CSR format.
    // Vertices: 0..n_vars-1 = variables, n_vars..n_vars+n_cls-1 = clauses
    int n_nodes = n_vars + n_cls;

    // Build adjacency lists
    std::vector<std::set<int>> adj(n_nodes);
    for (int ci = 0; ci < n_cls; ci++) {
        int clause_node = n_vars + ci;
        for (int v : clauses[ci]) {
            int var_node = v - 1; // 0-indexed
            adj[var_node].insert(clause_node);
            adj[clause_node].insert(var_node);
        }
    }

    // Convert to CSR
    std::vector<idx_t> xadj(n_nodes + 1);
    std::vector<idx_t> adjncy;
    xadj[0] = 0;
    for (int i = 0; i < n_nodes; i++) {
        for (int j : adj[i])
            adjncy.push_back(j);
        xadj[i + 1] = adjncy.size();
    }

    // Vertex weights: variables = 1, clauses = 1
    // (could experiment with different weights)
    std::vector<idx_t> vwgt(n_nodes, 1);

    // Options
    idx_t options[METIS_NOPTIONS];
    METIS_SetDefaultOptions(options);

    idx_t nvtxs = n_nodes;
    idx_t sepsize = 0;
    std::vector<idx_t> part(n_nodes);

    int ret = METIS_ComputeVertexSeparator(
        &nvtxs, xadj.data(), adjncy.data(),
        vwgt.data(), options, &sepsize, part.data());

    if (ret != METIS_OK) {
        fprintf(stderr, "METIS_ComputeVertexSeparator failed: %d\n", ret);
        return 1;
    }

    // Analyze results
    int n_left = 0, n_right = 0, n_sep = 0;
    int sep_vars = 0, sep_clauses = 0;
    int left_vars = 0, right_vars = 0;

    for (int i = 0; i < n_nodes; i++) {
        if (part[i] == 0) { n_left++; if (i < n_vars) left_vars++; }
        else if (part[i] == 1) { n_right++; if (i < n_vars) right_vars++; }
        else {
            n_sep++;
            if (i < n_vars) sep_vars++;
            else sep_clauses++;
        }
    }

    printf("\nMETIS vertex separator on bipartite incidence graph:\n");
    printf("  Left:      %d nodes (%d vars)\n", n_left, left_vars);
    printf("  Right:     %d nodes (%d vars)\n", n_right, right_vars);
    printf("  Separator: %d nodes (%d vars + %d clauses)\n",
           n_sep, sep_vars, sep_clauses);
    printf("  Balance:   %d/%d vars (%.2f)\n",
           left_vars, right_vars,
           left_vars > right_vars ?
           (float)left_vars / right_vars : (float)right_vars / left_vars);

    // Print separator elements
    printf("\n  Separator vars:");
    for (int i = 0; i < n_vars; i++)
        if (part[i] == 2) printf(" v%d", i + 1);
    printf("\n  Separator clauses:");
    for (int i = n_vars; i < n_nodes; i++)
        if (part[i] == 2) printf(" c%d", i - n_vars + 1);
    printf("\n");

    return 0;
}
