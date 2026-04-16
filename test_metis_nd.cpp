// Test METIS_NodeND on the bipartite incidence graph of a CNF formula.
// NodeND computes a nested dissection ordering — a hierarchy of vertex
// separators. We reconstruct and print the separator tree.
//
// Build: same flags as test_metis_sep.cpp

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <set>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
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

    // Build bipartite incidence graph in CSR format
    int n_nodes = n_vars + n_cls;
    std::vector<std::set<int>> adj(n_nodes);
    for (int ci = 0; ci < n_cls; ci++) {
        int clause_node = n_vars + ci;
        for (int v : clauses[ci]) {
            int var_node = v - 1;
            adj[var_node].insert(clause_node);
            adj[clause_node].insert(var_node);
        }
    }

    std::vector<idx_t> xadj(n_nodes + 1);
    std::vector<idx_t> adjncy;
    xadj[0] = 0;
    for (int i = 0; i < n_nodes; i++) {
        for (int j : adj[i])
            adjncy.push_back(j);
        xadj[i + 1] = adjncy.size();
    }

    // Vertex weights: variables = 1, clauses = 1
    std::vector<idx_t> vwgt(n_nodes, 1);

    idx_t options[METIS_NOPTIONS];
    METIS_SetDefaultOptions(options);

    idx_t nvtxs = n_nodes;
    std::vector<idx_t> perm(n_nodes), iperm(n_nodes);

    int ret = METIS_NodeND(&nvtxs, xadj.data(), adjncy.data(),
                           vwgt.data(), options, perm.data(), iperm.data());

    if (ret != METIS_OK) {
        fprintf(stderr, "METIS_NodeND failed: %d\n", ret);
        return 1;
    }

    // iperm[i] = position of original vertex i in the ND ordering.
    // In nested dissection, the ordering is:
    //   [left subtree vertices] [right subtree vertices] [separator vertices]
    // recursively. Vertices ordered LAST are in the top-level separator.

    printf("\nNested dissection ordering (last = top separator):\n");

    // The last vertices in iperm ordering are the top-level separator.
    // Let's find groups by looking at the ordering structure.

    // Reconstruct separator hierarchy using METIS_NodeNDP if available,
    // or manually by analyzing the ordering.

    // Simple analysis: print the last N vertices (top separator),
    // then analyze the split.

    // Sort vertices by their position in the ND ordering
    std::vector<std::pair<int, int>> order; // (position, original_vertex)
    for (int i = 0; i < n_nodes; i++)
        order.push_back({iperm[i], i});
    std::sort(order.begin(), order.end());

    // The ND ordering puts separator vertices LAST.
    // For a simple bisection: the top separator is the last group.
    // Let's find it by looking at the last few vertices.

    // Use METIS_ComputeVertexSeparator to get the top-level split,
    // then recurse manually to show the hierarchy.

    // Instead, let's just show the ordering in groups.
    printf("\nOrdering (position -> vertex type):\n");
    printf("  First 10: ");
    for (int i = 0; i < std::min(10, n_nodes); i++) {
        int v = order[i].second;
        if (v < n_vars) printf("v%d ", v + 1);
        else printf("c%d ", v - n_vars + 1);
    }
    printf("\n  Last 20:  ");
    for (int i = std::max(0, n_nodes - 20); i < n_nodes; i++) {
        int v = order[i].second;
        if (v < n_vars) printf("v%d ", v + 1);
        else printf("c%d ", v - n_vars + 1);
    }
    printf("\n");

    // Now use METIS_NodeNDP to get the actual separator tree sizes
    // NodeNDP gives the sizes array for the separator tree
    idx_t npes = 8; // request 8-way dissection (3 levels)
    std::vector<idx_t> sizes(2 * npes - 1, 0);
    std::vector<idx_t> perm2(n_nodes), iperm2(n_nodes);

    ret = METIS_NodeNDP(nvtxs, xadj.data(), adjncy.data(),
                        vwgt.data(), npes, options,
                        perm2.data(), iperm2.data(), sizes.data());

    if (ret != METIS_OK) {
        fprintf(stderr, "METIS_NodeNDP failed: %d\n", ret);
        // Fall back to just showing the ordering
    } else {
        // sizes[] has 2*npes-1 entries representing a binary tree:
        //   sizes[0..npes-1] = leaf partition sizes
        //   sizes[npes..2*npes-2] = separator sizes at each level
        // Tree structure (for npes=8):
        //   Level 0 (root separator): sizes[14]
        //   Level 1 separators: sizes[13], sizes[12]
        //   Level 2 separators: sizes[11], sizes[10], sizes[9], sizes[8]
        //   Leaves: sizes[0..7]

        printf("\nNested dissection tree (npes=%d, %d levels):\n", (int)npes, 3);

        // Root separator
        printf("  Root separator: %d nodes\n", (int)sizes[2 * npes - 2]);

        // Count vars vs clauses in each part using iperm2
        // Vertices are ordered: leaves first (left to right), then separators
        // bottom-up.
        int pos = 0;
        printf("\n  Leaf partitions:\n");
        for (int i = 0; i < (int)npes; i++) {
            int nv = 0, nc = 0;
            for (int j = 0; j < (int)sizes[i]; j++) {
                int v = perm2[pos + j]; // original vertex at this position
                if (v < n_vars) nv++;
                else nc++;
            }
            printf("    Leaf %d: %d nodes (%d vars + %d clauses)\n",
                   i, (int)sizes[i], nv, nc);
            pos += sizes[i];
        }

        printf("\n  Separators (bottom-up):\n");
        for (int i = (int)npes; i < 2 * (int)npes - 1; i++) {
            int nv = 0, nc = 0;
            for (int j = 0; j < (int)sizes[i]; j++) {
                int v = perm2[pos + j];
                if (v < n_vars) nv++;
                else nc++;
            }
            int level;
            if (i >= 2 * (int)npes - 2) level = 0;       // root
            else if (i >= 2 * (int)npes - 4) level = 1;
            else level = 2;
            printf("    Sep[%d] (level %d): %d nodes (%d vars + %d clauses)\n",
                   i, level, (int)sizes[i], nv, nc);
            pos += sizes[i];
        }

        // Summary
        int total_sep = 0;
        for (int i = (int)npes; i < 2 * (int)npes - 1; i++)
            total_sep += sizes[i];
        printf("\n  Total separator nodes: %d / %d (%.1f%%)\n",
               total_sep, n_nodes, 100.0 * total_sep / n_nodes);
    }

    return 0;
}
