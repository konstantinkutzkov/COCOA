/*
 * nd_hierarchy.cpp
 *
 * Build a precomputed separator hierarchy from a CNF formula's bipartite
 * incidence graph using recursive METIS_ComputeVertexSeparator calls.
 *
 * Each call bisects a subgraph into LEFT, RIGHT, SEPARATOR. The separator
 * is stored; LEFT and RIGHT are recursed on (if large enough).
 */

#include "nd_hierarchy.h"
#include <metis.h>
#include <cstdio>
#include <cassert>
#include <queue>

using namespace std;

// Internal: a subgraph described by its vertex set (indices into the
// full graph). We build METIS CSR for this subset and bisect it.
struct Subgraph {
  vector<int> vertices;        // indices into the full graph
  vector<vector<int>> adj;     // adjacency lists (using FULL graph indices)
  int n_vars_in_full;          // total vars in full graph (for var/clause distinction)
};

// Compute vertex separator for a subgraph. Returns separator vertex
// indices (in full graph) and partitions vertices into left/right.
static bool bisect_subgraph(
    const Subgraph &sg,
    vector<int> &sep_verts,     // output: full-graph indices of separator
    vector<int> &left_verts,    // output: full-graph indices of left part
    vector<int> &right_verts)   // output: full-graph indices of right part
{
  int n = sg.vertices.size();
  if (n < 4) return false;

  // Map full-graph indices to local 0..n-1
  unordered_map<int, int> to_local;
  for (int i = 0; i < n; i++)
    to_local[sg.vertices[i]] = i;

  // Build CSR for the subgraph
  vector<idx_t> xadj(n + 1);
  vector<idx_t> adjncy;
  xadj[0] = 0;
  for (int i = 0; i < n; i++) {
    int v = sg.vertices[i];
    for (int u : sg.adj[v]) {
      auto it = to_local.find(u);
      if (it != to_local.end())
        adjncy.push_back(it->second);
    }
    xadj[i + 1] = adjncy.size();
  }

  vector<idx_t> vwgt(n, 1);
  idx_t options[METIS_NOPTIONS];
  METIS_SetDefaultOptions(options);

  idx_t nvtxs = n;
  idx_t sepsize = 0;
  vector<idx_t> part(n);

  int ret = METIS_ComputeVertexSeparator(
      &nvtxs, xadj.data(), adjncy.data(),
      vwgt.data(), options, &sepsize, part.data());

  if (ret != METIS_OK) return false;

  sep_verts.clear();
  left_verts.clear();
  right_verts.clear();

  for (int i = 0; i < n; i++) {
    int full_idx = sg.vertices[i];
    if (part[i] == 0) left_verts.push_back(full_idx);
    else if (part[i] == 1) right_verts.push_back(full_idx);
    else sep_verts.push_back(full_idx);
  }

  // Need both sides non-empty
  if (left_verts.empty() || right_verts.empty()) return false;

  return true;
}

void NDHierarchy::build(
    int n_vars,
    const vector<pair<unsigned, vector<unsigned>>> &clauses,
    int /* target_npes — unused, tree depth is automatic */)
{
  valid = false;
  int n_cls = clauses.size();
  int n_full = n_vars + n_cls;

  if (n_full < 4) return;

  // Build full adjacency lists (indexed by full graph index)
  // Vertices: 0..n_vars-1 = variables, n_vars..n_full-1 = clauses
  vector<vector<int>> full_adj(n_full);
  for (int ci = 0; ci < n_cls; ci++) {
    int clause_gidx = n_vars + ci;
    for (unsigned var_id : clauses[ci].second) {
      if (var_id < 1 || (int)var_id > n_vars) continue;
      int var_gidx = var_id - 1;  // 0-indexed
      full_adj[var_gidx].push_back(clause_gidx);
      full_adj[clause_gidx].push_back(var_gidx);
    }
  }

  // Maps for full graph index ↔ variable/clause IDs
  auto gidx_to_cutnode = [&](int gidx) -> CutNode {
    if (gidx < n_vars)
      return CutNode(CutNode::VAR, gidx + 1);  // 1-indexed var ID
    else
      return CutNode(CutNode::CLAUSE, clauses[gidx - n_vars].first);
  };

  // Dynamic tree built via recursive bisection.
  // Use vectors for tree storage; grow as needed.
  separator.clear();
  left_child.clear();
  right_child.clear();
  var_leaf.assign(n_vars + 1, -1);
  clause_leaf.clear();
  leaf_lo.clear();
  leaf_hi.clear();

  // BFS/DFS queue: (subgraph vertex set, tree node index)
  // We build the tree breadth-first.
  struct WorkItem {
    vector<int> vertices;
    int tree_node;
  };

  int min_sep_vars = 6;  // don't bisect components with < 6 variables

  // Allocate root node
  int next_node = 0;
  auto alloc_node = [&]() {
    int id = next_node++;
    separator.push_back({});
    left_child.push_back(-1);
    right_child.push_back(-1);
    leaf_lo.push_back(-1);
    leaf_hi.push_back(-1);
    return id;
  };

  int root_id = alloc_node();
  int next_leaf = 0;

  queue<WorkItem> work;
  {
    vector<int> all_verts(n_full);
    for (int i = 0; i < n_full; i++) all_verts[i] = i;
    work.push({all_verts, root_id});
  }

  while (!work.empty()) {
    auto item = work.front();
    work.pop();

    // Count variables in this subgraph
    int nv = 0;
    for (int v : item.vertices)
      if (v < n_vars) nv++;

    if (nv < min_sep_vars) {
      // Too small — make this a leaf
      int leaf_id = next_leaf++;
      leaf_lo[item.tree_node] = leaf_id;
      leaf_hi[item.tree_node] = leaf_id;
      for (int v : item.vertices) {
        CutNode nd = gidx_to_cutnode(v);
        if (nd.kind == CutNode::VAR)
          var_leaf[nd.id] = leaf_id;
        else
          clause_leaf[nd.id] = leaf_id;
      }
      continue;
    }

    // Build subgraph
    Subgraph sg;
    sg.vertices = item.vertices;
    sg.adj.resize(n_full);  // sparse: only entries for vertices in sg
    for (int v : item.vertices)
      sg.adj[v] = full_adj[v];
    sg.n_vars_in_full = n_vars;

    vector<int> sep_v, left_v, right_v;
    bool ok = bisect_subgraph(sg, sep_v, left_v, right_v);

    if (!ok) {
      // Bisection failed — make leaf
      int leaf_id = next_leaf++;
      leaf_lo[item.tree_node] = leaf_id;
      leaf_hi[item.tree_node] = leaf_id;
      for (int v : item.vertices) {
        CutNode nd = gidx_to_cutnode(v);
        if (nd.kind == CutNode::VAR)
          var_leaf[nd.id] = leaf_id;
        else
          clause_leaf[nd.id] = leaf_id;
      }
      continue;
    }

    // Store separator at this tree node
    for (int v : sep_v)
      separator[item.tree_node].push_back(gidx_to_cutnode(v));

    // Assign separator vertices to a leaf (for mapToChild: use left side)
    // They'll be consumed during branching, but need a leaf assignment
    // so that if they show up in a sub-component they map correctly.

    // Allocate children
    int lc = alloc_node();
    int rc = alloc_node();
    left_child[item.tree_node] = lc;
    right_child[item.tree_node] = rc;

    // Queue children for further bisection
    work.push({left_v, lc});
    work.push({right_v, rc});
  }

  // Assign separator vertices to a leaf in their left child's range
  // (after all leaves are assigned). Do a post-pass.
  for (int i = 0; i < next_node; i++) {
    if (separator[i].empty()) continue;
    int lc = left_child[i];
    int assigned_leaf = (lc >= 0 && leaf_lo[lc] >= 0) ? leaf_lo[lc] : 0;
    for (const auto &nd : separator[i]) {
      if (nd.kind == CutNode::VAR) {
        if (nd.id < var_leaf.size()) var_leaf[nd.id] = assigned_leaf;
      } else {
        clause_leaf[nd.id] = assigned_leaf;
      }
    }
    // Update this node's leaf range
    if (lc >= 0 && right_child[i] >= 0) {
      leaf_lo[i] = leaf_lo[lc];
      leaf_hi[i] = (leaf_hi[right_child[i]] >= 0) ?
                    leaf_hi[right_child[i]] : leaf_hi[lc];
    }
  }

  npes = next_leaf;
  n_nodes = next_node;
  valid = true;

  // Summary
  int total_sep = 0, internal = 0;
  for (int i = 0; i < next_node; i++)
    if (!separator[i].empty()) { total_sep += separator[i].size(); internal++; }
  fprintf(stderr, "NDHierarchy: %d tree nodes, %d internal (sep), %d leaves, "
          "%d total sep elements\n", next_node, internal, next_leaf, total_sep);
}

vector<CutNode> NDHierarchy::lookupSeparator(
    const vector<unsigned> &active_var_ids,
    int hint_node,
    int *out_node) const
{
  if (!valid) return {};

  int node = (hint_node >= 0 && hint_node < n_nodes) ? hint_node : 0;

  // If this node has no separator (leaf), return empty
  if (node >= n_nodes || separator[node].empty())
    return {};

  if (out_node) *out_node = node;
  return separator[node];
}

int NDHierarchy::mapToChild(
    int parent_node,
    const vector<unsigned> &active_var_ids) const
{
  if (!valid || parent_node < 0 || parent_node >= n_nodes) return -1;

  int lc = left_child[parent_node];
  int rc = right_child[parent_node];
  if (lc < 0 || rc < 0) return -1;

  int left_lo_val = leaf_lo[lc], left_hi_val = leaf_hi[lc];
  if (left_lo_val < 0) return -1;

  bool any_left = false, any_right = false;
  for (unsigned var_id : active_var_ids) {
    if (var_id >= var_leaf.size()) continue;
    int leaf = var_leaf[var_id];
    if (leaf < 0) continue;
    if (leaf >= left_lo_val && leaf <= left_hi_val)
      any_left = true;
    else
      any_right = true;
    if (any_left && any_right) break;
  }

  if (any_left && !any_right) return lc;
  if (any_right && !any_left) return rc;
  return -1;  // mixed
}
