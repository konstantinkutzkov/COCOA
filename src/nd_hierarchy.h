/*
 * nd_hierarchy.h
 *
 * Precomputed nested-dissection hierarchy via METIS_NodeNDP on the
 * bipartite incidence graph. Provides a separator tree that can be
 * looked up in O(1) per recursion level during solving.
 *
 * Tree layout (for npes leaf partitions, 2*npes-1 total nodes):
 *   Nodes 0..npes-1          = leaf partitions (no separator)
 *   Nodes npes..2*npes-2     = internal (separator) nodes
 *   Node  2*npes-2           = root
 *
 * Each internal node stores the list of separator CutNodes.
 * Each vertex (variable or clause) is assigned to a leaf partition.
 * To find the separator for a runtime component: look up the deepest
 * internal node whose leaf set covers the component's leaves.
 */

#ifndef ND_HIERARCHY_H_
#define ND_HIERARCHY_H_

#include <vector>
#include <unordered_map>
#include <set>
#include <algorithm>
#include "separator.h"

struct NDHierarchy {
  int npes = 0;          // number of leaf partitions (power of 2)
  int n_nodes = 0;       // 2*npes - 1
  bool valid = false;

  // For each tree node (0..n_nodes-1):
  //   separator[i] = CutNodes in separator i (empty for leaves)
  std::vector<std::vector<CutNode>> separator;

  // For each tree node: range of leaf indices it covers [leaf_lo, leaf_hi]
  std::vector<int> leaf_lo, leaf_hi;

  // For each tree node: left and right children (-1 for leaves)
  std::vector<int> left_child, right_child;

  // For each original variable (1-indexed): leaf partition index
  // -1 = not in the graph (assigned during preprocessing)
  std::vector<int> var_leaf;

  // For each original clause offset: leaf partition index
  std::unordered_map<unsigned, int> clause_leaf;

  int root() const { return 0; }  // dynamic tree: root allocated first

  // Build hierarchy from a CNF formula's incidence graph.
  //
  // Long clauses (size >= 3) contribute a clause-node + one edge per
  // literal (bipartite pattern). Binary clauses MUST be supplied here
  // too: they are rendered as direct var-var edges in the same graph
  // (non-bipartite — METIS handles this fine). Omitting binaries was
  // a latent bug: it gave METIS an incomplete connectivity graph, so
  // the produced separator could leave binary-clause connectivity
  // intact across its two sides, violating the separator invariant
  // for any formula with binary clauses.
  //
  // Unit clauses are not needed (a unit fixes a single variable; it
  // doesn't contribute connectivity between distinct variables).
  //
  // Arguments:
  //   n_vars         - number of variables (1-indexed)
  //   long_clauses   - list of (clause_offset, list of variable IDs)
  //                    for clauses of size >= 3
  //   binary_pairs   - list of (var_a, var_b) for binary clauses; each
  //                    pair represents one binary clause (caller must
  //                    deduplicate)
  void build(int n_vars,
             const std::vector<std::pair<unsigned, std::vector<unsigned>>> &long_clauses,
             const std::vector<std::pair<unsigned, unsigned>> &binary_pairs,
             int target_npes = 8);

  // Look up the best separator for a component described by its active
  // variable IDs. Returns the separator CutNodes, or empty if none found.
  // Also returns the hierarchy node ID via *out_node (for child lookups).
  std::vector<CutNode> lookupSeparator(
      const std::vector<unsigned> &active_var_ids,
      int hint_node = -1,
      int *out_node = nullptr) const;

  // Given a hierarchy node that was just consumed, and a sub-component's
  // active variable IDs, determine which child node to use next.
  int mapToChild(int parent_node,
                 const std::vector<unsigned> &active_var_ids) const;
};

#endif /* ND_HIERARCHY_H_ */
