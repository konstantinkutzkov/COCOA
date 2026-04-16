/*
 * nd_hierarchy.cpp
 *
 * Build a precomputed nested-dissection separator hierarchy from a CNF
 * formula's bipartite incidence graph using METIS_NodeNDP.
 */

#include "nd_hierarchy.h"
#include <metis.h>
#include <cstdio>
#include <cassert>

using namespace std;

void NDHierarchy::build(
    int n_vars,
    const vector<pair<unsigned, vector<unsigned>>> &clauses,
    int target_npes)
{
  valid = false;
  int n_cls = clauses.size();
  int n_graph_nodes = n_vars + n_cls;

  if (n_graph_nodes < 4) return;

  npes = target_npes;
  n_nodes = 2 * npes - 1;

  // Build CSR adjacency for the bipartite incidence graph.
  // Vertices: 0..n_vars-1 = variables, n_vars..n_vars+n_cls-1 = clauses.

  // Map variable IDs (1-indexed) to graph indices (0-indexed)
  // Variables might have gaps after preprocessing, so map explicitly.
  unordered_map<unsigned, int> var_to_gidx;
  vector<unsigned> gidx_to_var(n_vars);  // graph index → variable ID
  // We assume variables are 1..n_vars (after preprocessing, some might
  // be inactive, but we build the graph for ALL variables — inactive
  // ones will just have no edges and end up as trivial leaves).

  for (int i = 0; i < n_vars; i++) {
    var_to_gidx[i + 1] = i;
    gidx_to_var[i] = i + 1;
  }

  // Build adjacency lists
  vector<vector<int>> adj(n_graph_nodes);
  for (int ci = 0; ci < n_cls; ci++) {
    int clause_gidx = n_vars + ci;
    for (unsigned var_id : clauses[ci].second) {
      auto it = var_to_gidx.find(var_id);
      if (it == var_to_gidx.end()) continue;
      int var_gidx = it->second;
      adj[var_gidx].push_back(clause_gidx);
      adj[clause_gidx].push_back(var_gidx);
    }
  }

  // Convert to CSR
  vector<idx_t> xadj(n_graph_nodes + 1);
  vector<idx_t> adjncy;
  xadj[0] = 0;
  for (int i = 0; i < n_graph_nodes; i++) {
    for (int j : adj[i])
      adjncy.push_back(j);
    xadj[i + 1] = adjncy.size();
  }

  // Vertex weights (all 1 for now)
  vector<idx_t> vwgt(n_graph_nodes, 1);

  idx_t options[METIS_NOPTIONS];
  METIS_SetDefaultOptions(options);

  idx_t nvtxs = n_graph_nodes;
  vector<idx_t> perm(n_graph_nodes), iperm(n_graph_nodes);
  vector<idx_t> sizes(2 * npes - 1, 0);

  int ret = METIS_NodeNDP(nvtxs, xadj.data(), adjncy.data(),
                          vwgt.data(), (idx_t)npes, options,
                          perm.data(), iperm.data(), sizes.data());

  if (ret != METIS_OK) {
    fprintf(stderr, "METIS_NodeNDP failed (%d)\n", ret);
    return;
  }

  // Parse the output.
  // perm[pos] = original vertex at position pos in ND ordering.
  // ND ordering: [leaf 0 verts][leaf 1 verts]...[leaf npes-1 verts]
  //              [sep npes][sep npes+1]...[sep 2*npes-2 (root)]

  // Allocate tree structures
  separator.resize(n_nodes);
  leaf_lo.resize(n_nodes, -1);
  leaf_hi.resize(n_nodes, -1);
  left_child.resize(n_nodes, -1);
  right_child.resize(n_nodes, -1);

  // Assign each vertex to its leaf partition or separator
  var_leaf.assign(n_vars + 1, -1);  // 1-indexed
  clause_leaf.clear();

  int pos = 0;

  // Process leaves
  for (int leaf = 0; leaf < npes; leaf++) {
    leaf_lo[leaf] = leaf;
    leaf_hi[leaf] = leaf;
    for (int j = 0; j < (int)sizes[leaf]; j++) {
      int v = perm[pos + j];
      if (v < n_vars) {
        var_leaf[gidx_to_var[v]] = leaf;
      } else {
        int ci = v - n_vars;
        clause_leaf[clauses[ci].first] = leaf;
      }
    }
    pos += sizes[leaf];
  }

  // Process separators (bottom-up)
  // Build tree structure. For npes = 2^k:
  //   Level k-1 (bottom internal): nodes npes..npes+npes/2-1
  //     Each pairs two adjacent leaves: node npes+i covers leaves 2i, 2i+1
  //   Level k-2: nodes npes+npes/2..npes+npes/2+npes/4-1
  //     Each pairs two level-(k-1) nodes
  //   ...
  //   Level 0 (root): node 2*npes-2

  // First, build separator CutNode lists from perm
  for (int sep_idx = npes; sep_idx < n_nodes; sep_idx++) {
    for (int j = 0; j < (int)sizes[sep_idx]; j++) {
      int v = perm[pos + j];
      if (v < n_vars) {
        separator[sep_idx].push_back(
            CutNode(CutNode::VAR, gidx_to_var[v]));
      } else {
        int ci = v - n_vars;
        separator[sep_idx].push_back(
            CutNode(CutNode::CLAUSE, clauses[ci].first));
      }
    }
    pos += sizes[sep_idx];
  }

  // Build tree links.
  // The internal nodes are laid out level by level, bottom-up.
  // Level d (deepest): npes/2 nodes starting at index npes
  // Level d-1: npes/4 nodes starting at index npes + npes/2
  // ...
  // Level 0: 1 node (root) at index 2*npes-2

  int level_start = npes;     // first internal node of current level
  int level_size = npes / 2;  // number of internal nodes at current level
  int child_start = 0;        // first node that children come from

  while (level_size > 0) {
    for (int i = 0; i < level_size; i++) {
      int node = level_start + i;
      int lc = child_start + 2 * i;
      int rc = child_start + 2 * i + 1;
      left_child[node] = lc;
      right_child[node] = rc;

      // Leaf range = union of children's ranges
      leaf_lo[node] = leaf_lo[lc];
      leaf_hi[node] = leaf_hi[rc];
    }
    child_start = level_start;
    level_start += level_size;
    level_size /= 2;
  }

  // Assign leaf indices to separator vertices.
  // A separator variable at internal node N covers leaves [leaf_lo[N],
  // leaf_hi[N]]. For mapToChild to work, we assign it to leaf_lo[N]
  // — any leaf in the range works, the subtree membership check only
  // needs the leaf to be within the correct child's range.
  for (int sep_idx = npes; sep_idx < n_nodes; sep_idx++) {
    int assigned_leaf = leaf_lo[sep_idx];
    for (const auto &nd : separator[sep_idx]) {
      if (nd.kind == CutNode::VAR) {
        if (nd.id < var_leaf.size()) var_leaf[nd.id] = assigned_leaf;
      } else {
        clause_leaf[nd.id] = assigned_leaf;
      }
    }
  }

  valid = true;

  // Summary
  int total_sep = 0;
  for (int i = npes; i < n_nodes; i++)
    total_sep += separator[i].size();
  fprintf(stderr, "NDHierarchy: %d leaves, %d separators, %d total sep elements, "
          "%d graph nodes\n", npes, npes - 1, total_sep, n_graph_nodes);
}

vector<CutNode> NDHierarchy::lookupSeparator(
    const vector<unsigned> &active_var_ids,
    int hint_node,
    int *out_node) const
{
  if (!valid) return {};

  // Determine which leaves the active variables belong to
  set<int> active_leaves;
  for (unsigned var_id : active_var_ids) {
    if (var_id < var_leaf.size() && var_leaf[var_id] >= 0)
      active_leaves.insert(var_leaf[var_id]);
  }

  static unsigned long hits = 0, misses_leaf = 0, misses_empty = 0, misses_node = 0;

  if (active_leaves.size() <= 1) {
    misses_leaf++;
    if ((misses_leaf + hits) % 1000 == 0)
      fprintf(stderr, "ND lookup: hits=%lu miss_leaf=%lu miss_empty=%lu miss_node=%lu\n",
              hits, misses_leaf, misses_empty, misses_node);
    return {};
  }

  // Find the deepest internal node that covers ALL active leaves
  // and has a non-empty separator.
  // Start from hint_node if given, otherwise from root.
  int node = (hint_node >= 0 && hint_node < n_nodes) ? hint_node : root();

  // Navigate to find the best separator
  // The node should cover all active_leaves: leaf_lo[node]..leaf_hi[node]
  // must include all of them.
  if (node < npes) { misses_node++; return {}; }

  if (separator[node].empty()) { misses_empty++; return {}; }

  hits++;
  if (hits % 1000 == 0)
    fprintf(stderr, "ND lookup: hits=%lu miss_leaf=%lu miss_empty=%lu miss_node=%lu\n",
            hits, misses_leaf, misses_empty, misses_node);
  if (out_node) *out_node = node;
  return separator[node];
}

int NDHierarchy::mapToChild(
    int parent_node,
    const vector<unsigned> &active_var_ids) const
{
  if (!valid || parent_node < npes) return -1;

  int lc = left_child[parent_node];
  int rc = right_child[parent_node];
  if (lc < 0 || rc < 0) return -1;

  // Check which child's leaf range contains the component's variables
  int left_lo = leaf_lo[lc], left_hi = leaf_hi[lc];

  bool any_left = false, any_right = false;
  for (unsigned var_id : active_var_ids) {
    if (var_id >= var_leaf.size()) continue;
    int leaf = var_leaf[var_id];
    if (leaf < 0) continue;
    if (leaf >= left_lo && leaf <= left_hi)
      any_left = true;
    else
      any_right = true;
    if (any_left && any_right) break;  // mixed — shouldn't happen
  }

  static unsigned long map_ok = 0;
  if (any_left && !any_right) { map_ok++; return lc; }
  if (any_right && !any_left) { map_ok++; return rc; }

  // Mixed or unknown — can't map cleanly
  static unsigned long map_fail = 0;
  map_fail++;
  if (map_fail % 100 == 0)
    fprintf(stderr, "mapToChild: ok=%lu fail=%lu (parent=%d any_left=%d any_right=%d)\n",
            map_ok, map_fail, parent_node, any_left, any_right);
  return -1;
}

// Add success counter at the success paths above — actually let me add inline

