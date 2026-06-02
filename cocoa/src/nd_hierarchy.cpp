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
#include <chrono>
#include <queue>
#include <functional>
#include <utility>
#include <iostream>
#include <string>

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
    const vector<pair<unsigned, unsigned>> &binary_pairs,
    bool vars_only,
    int /* target_npes — unused, tree depth is automatic */)
{
  valid = false;
  int n_cls = clauses.size();
  // In vars_only mode the graph contains only variable nodes; long
  // clauses contribute clique edges among their variables. In
  // bipartite mode there's an aux node per long clause.
  int n_full = vars_only ? n_vars : (n_vars + n_cls);

  if (n_full < 4) return;

  // Build adjacency lists (indexed by full graph index).
  // Bipartite mode: vertices 0..n_vars-1 = variables, n_vars..n_full-1
  // = long-clause nodes. Binary clauses are NOT represented as
  // separate nodes; they contribute a direct edge between their two
  // variable nodes. This is not strictly bipartite but METIS handles
  // general undirected graphs.
  // Vars-only mode: only variable nodes; long clauses contribute one
  // edge per pair of variables in the clause (clique). Multi-edges
  // between vars co-occurring in multiple clauses are allowed —
  // METIS treats them as weighted connectivity which is the right
  // semantic.
  vector<vector<int>> full_adj(n_full);
  if (vars_only) {
    // Long clauses → cliques on their variables.
    for (int ci = 0; ci < n_cls; ci++) {
      const auto &cl_vars = clauses[ci].second;
      const size_t k = cl_vars.size();
      for (size_t i = 0; i < k; i++) {
        if (cl_vars[i] < 1 || (int)cl_vars[i] > n_vars) continue;
        int gi = (int)cl_vars[i] - 1;
        for (size_t j = i + 1; j < k; j++) {
          if (cl_vars[j] < 1 || (int)cl_vars[j] > n_vars) continue;
          int gj = (int)cl_vars[j] - 1;
          full_adj[gi].push_back(gj);
          full_adj[gj].push_back(gi);
        }
      }
    }
  } else {
    // Bipartite: var-clause edges via aux clause nodes.
    for (int ci = 0; ci < n_cls; ci++) {
      int clause_gidx = n_vars + ci;
      for (unsigned var_id : clauses[ci].second) {
        if (var_id < 1 || (int)var_id > n_vars) continue;
        int var_gidx = var_id - 1;  // 0-indexed
        full_adj[var_gidx].push_back(clause_gidx);
        full_adj[clause_gidx].push_back(var_gidx);
      }
    }
  }
  // Binary clauses: direct var-var edges (both modes).
  for (const auto &pr : binary_pairs) {
    unsigned a = pr.first, b = pr.second;
    if (a < 1 || (int)a > n_vars) continue;
    if (b < 1 || (int)b > n_vars) continue;
    int ga = a - 1, gb = b - 1;
    full_adj[ga].push_back(gb);
    full_adj[gb].push_back(ga);
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

  int min_sep_vars = 2;  // bisect all the way down — no Dinic's fallback needed

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

  // Helper: turn a WorkItem into a leaf. All its vertices land in one
  // partition; next_leaf is consumed.
  auto make_leaf = [&](const WorkItem &item) {
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
  };

  // Helper: connected components of the subgraph induced by `verts`,
  // using `full_adj` restricted to those vertices. O(|V|+|E|) per call.
  // Returns one vector per component; each component is a list of
  // full-graph vertex indices.
  vector<bool> in_set_buf(n_full, false);
  vector<bool> visited_buf(n_full, false);
  auto connected_components = [&](const vector<int> &verts)
      -> vector<vector<int>> {
    for (int v : verts) in_set_buf[v] = true;
    // visited_buf is reused across calls — only touch entries in verts.
    vector<vector<int>> comps;
    for (int start : verts) {
      if (visited_buf[start]) continue;
      vector<int> comp;
      std::queue<int> q;
      q.push(start);
      visited_buf[start] = true;
      while (!q.empty()) {
        int v = q.front(); q.pop();
        comp.push_back(v);
        for (int u : full_adj[v]) {
          if (in_set_buf[u] && !visited_buf[u]) {
            visited_buf[u] = true;
            q.push(u);
          }
        }
      }
      comps.push_back(std::move(comp));
    }
    // Clean up the two scratch bitmaps so they're zero for the next call.
    for (int v : verts) { in_set_buf[v] = false; visited_buf[v] = false; }
    return comps;
  };

  while (!work.empty()) {
    auto item = work.front();
    work.pop();

    // Count variables in this subgraph
    int nv = 0;
    for (int v : item.vertices)
      if (v < n_vars) nv++;

    if (nv < min_sep_vars) {
      // Too small — make this a leaf
      make_leaf(item);
      continue;
    }

    // Connectivity split: if the induced subgraph is disconnected, we
    // must NOT pass it to METIS. METIS_ComputeVertexSeparator can return
    // METIS_OK with sepsize=0 on a disconnected graph (the two components
    // are already separated — no separator vertex needed). That zero-sep
    // "success" leaves us with an internal node whose separator is empty
    // but whose children have real subtrees — a "pass-through" state.
    //
    // Pass-throughs are represented explicitly: the current node gets
    // empty separator and two children — LEFT covers the first connected
    // component, RIGHT covers the union of the rest. If the rest is
    // still disconnected, it will split itself again on the next
    // dequeue. Consumers use (left_child<0 && right_child<0) as the
    // leaf test rather than separator-emptiness.
    auto comps = connected_components(item.vertices);
    if (comps.size() >= 2) {
      int lc = alloc_node();
      int rc = alloc_node();
      left_child[item.tree_node] = lc;
      right_child[item.tree_node] = rc;
      // separator[item.tree_node] stays empty: this is a pass-through.

      // LEFT = first connected component.
      work.push({std::move(comps[0]), lc});

      // RIGHT = union of the remaining components.
      vector<int> rest;
      size_t rest_size = 0;
      for (size_t i = 1; i < comps.size(); i++)
        rest_size += comps[i].size();
      rest.reserve(rest_size);
      for (size_t i = 1; i < comps.size(); i++)
        for (int v : comps[i]) rest.push_back(v);
      work.push({std::move(rest), rc});
      continue;
    }

    // Connected — safe to call METIS for a real vertex separator.
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
      make_leaf(item);
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

  // Post-pass 1: renumber leaves in DFS order so each subtree's leaves
  // form a contiguous range [leaf_lo..leaf_hi]. BFS order (as built above)
  // interleaves sibling subtrees' leaves, breaking range queries.
  std::vector<int> old_to_new(next_leaf, -1);
  int dfs_leaf_id = 0;
  std::function<void(int)> dfs = [&](int node) {
    if (node < 0) return;
    // Leafness is determined by having no children, NOT by having an
    // empty separator. Pass-through nodes (inserted for disconnected
    // subgraphs) have empty separator but real children; they must
    // recurse, not remap.
    bool is_leaf = (left_child[node] < 0 && right_child[node] < 0);
    if (is_leaf) {
      int old_id = leaf_lo[node];
      if (old_id >= 0 && old_id < (int)old_to_new.size()) {
        if (old_to_new[old_id] < 0)
          old_to_new[old_id] = dfs_leaf_id++;
        leaf_lo[node] = leaf_hi[node] = old_to_new[old_id];
      }
      return;
    }
    dfs(left_child[node]);
    dfs(right_child[node]);
  };
  dfs(0);

  // Update var_leaf and clause_leaf with the remapped IDs
  for (size_t i = 0; i < var_leaf.size(); i++) {
    int old_id = var_leaf[i];
    if (old_id >= 0 && old_id < (int)old_to_new.size() && old_to_new[old_id] >= 0)
      var_leaf[i] = old_to_new[old_id];
  }
  for (auto &kv : clause_leaf) {
    int old_id = kv.second;
    if (old_id >= 0 && old_id < (int)old_to_new.size() && old_to_new[old_id] >= 0)
      kv.second = old_to_new[old_id];
  }

  // Post-pass 2: propagate leaf ranges up from leaves to root (bottom-up).
  // Process internal nodes (including pass-throughs with empty separator)
  // — only genuine leaves are already done and must be skipped.
  for (int i = next_node - 1; i >= 0; i--) {
    bool is_leaf = (left_child[i] < 0 && right_child[i] < 0);
    if (is_leaf) continue;  // leaf — already set above
    int lc = left_child[i];
    int rc = right_child[i];
    if (lc >= 0 && rc >= 0 && leaf_lo[lc] >= 0 && leaf_lo[rc] >= 0) {
      leaf_lo[i] = std::min(leaf_lo[lc], leaf_lo[rc]);
      leaf_hi[i] = std::max(leaf_hi[lc], leaf_hi[rc]);
    } else if (lc >= 0 && leaf_lo[lc] >= 0) {
      leaf_lo[i] = leaf_lo[lc];
      leaf_hi[i] = leaf_hi[lc];
    } else if (rc >= 0 && leaf_lo[rc] >= 0) {
      leaf_lo[i] = leaf_lo[rc];
      leaf_hi[i] = leaf_hi[rc];
    }
    // Assign separator vertices to leaf_lo of this node's subtree.
    // Empty for pass-through nodes; the loop is then a no-op.
    int assigned_leaf = (leaf_lo[i] >= 0) ? leaf_lo[i] : 0;
    for (const auto &nd : separator[i]) {
      if (nd.kind == CutNode::VAR) {
        if (nd.id < var_leaf.size()) var_leaf[nd.id] = assigned_leaf;
      } else {
        clause_leaf[nd.id] = assigned_leaf;
      }
    }
  }

  npes = next_leaf;
  n_nodes = next_node;
  valid = true;

  // Summary. Classify each node:
  //   leaf        = no children
  //   passthrough = has children, empty separator (connectivity split)
  //   internal    = has children, non-empty separator (METIS bisection)
  int total_sep = 0, internal = 0, passthrough = 0;
  int max_sep = 0;
  int sep_buckets[7] = {0,0,0,0,0,0,0};  // [0]=1-2, [1]=3-4, [2]=5-7, [3]=8-15, [4]=16-31, [5]=32-63, [6]=64+
  // Per-element-kind breakdown: how many separator elements are VARs
  // vs CLAUSEs across the whole hierarchy, and the kind of the
  // largest separator node.
  int total_sep_vars = 0, total_sep_clauses = 0;
  int max_sep_vars = 0, max_sep_clauses = 0;  // for the max_sep node
  // Build cl_ofs → length map from the long_clauses input passed to build().
  // Lets the summary print a histogram of separator-clause lengths.
  std::unordered_map<unsigned, unsigned> cl_len;
  cl_len.reserve(clauses.size() * 2);
  for (const auto &c : clauses) cl_len[c.first] = (unsigned)c.second.size();
  // Histogram of clause lengths across ALL separator clause elements.
  // Buckets: [3, 4, 5, 6, 7, 8, 9, 10-15, 16+]. Index 0 = length 3.
  int cl_len_buckets[9] = {0,0,0,0,0,0,0,0,0};
  auto bucket_idx = [](unsigned len) -> int {
    if (len <= 3) return 0;
    if (len <= 9) return (int)len - 3;   // len 4→1, 5→2, ..., 9→6
    if (len <= 15) return 7;
    return 8;
  };
  for (int i = 0; i < next_node; i++) {
    bool is_leaf = (left_child[i] < 0 && right_child[i] < 0);
    if (is_leaf) continue;
    if (separator[i].empty()) passthrough++;
    else {
      int s = (int)separator[i].size();
      total_sep += s; internal++;
      int sv = 0, sc = 0;
      for (const auto &e : separator[i]) {
        if (e.kind == CutNode::VAR) sv++;
        else {
          sc++;
          auto it = cl_len.find(e.id);
          if (it != cl_len.end()) cl_len_buckets[bucket_idx(it->second)]++;
        }
      }
      total_sep_vars += sv;
      total_sep_clauses += sc;
      if (s > max_sep) {
        max_sep = s;
        max_sep_vars = sv;
        max_sep_clauses = sc;
      }
      if (s <= 2) sep_buckets[0]++;
      else if (s <= 4) sep_buckets[1]++;
      else if (s <= 7) sep_buckets[2]++;
      else if (s <= 15) sep_buckets[3]++;
      else if (s <= 31) sep_buckets[4]++;
      else if (s <= 63) sep_buckets[5]++;
      else sep_buckets[6]++;
    }
  }
  fprintf(stderr, "NDHierarchy: %d tree nodes, %d internal (sep), "
          "%d passthrough, %d leaves, %d total sep elements "
          "(vars=%d clauses=%d), "
          "max_sep=%d (vars=%d clauses=%d), "
          "sep_buckets=[1-2:%d, 3-4:%d, 5-7:%d, 8-15:%d, 16-31:%d, 32-63:%d, 64+:%d], "
          "sep_clause_len=[3:%d, 4:%d, 5:%d, 6:%d, 7:%d, 8:%d, 9:%d, 10-15:%d, 16+:%d]\n",
          next_node, internal, passthrough, next_leaf,
          total_sep, total_sep_vars, total_sep_clauses,
          max_sep, max_sep_vars, max_sep_clauses,
          sep_buckets[0], sep_buckets[1], sep_buckets[2], sep_buckets[3],
          sep_buckets[4], sep_buckets[5], sep_buckets[6],
          cl_len_buckets[0], cl_len_buckets[1], cl_len_buckets[2],
          cl_len_buckets[3], cl_len_buckets[4], cl_len_buckets[5],
          cl_len_buckets[6], cl_len_buckets[7], cl_len_buckets[8]);

  // Weighted-degree centrality focused on ROOT separator vars only.
  // Reasoning: on dense instances most vars become separator vars in
  // SOME internal node (e.g., 263 of 318 on t1_105). Scoring all of them
  // dilutes the signal. The root separator is the cut whose removal
  // produces the LARGEST disconnection, so its vars are where centrality
  // information matters most. Other separator vars get 0.
  //
  // For each root separator var v: score = number of edges in the primal
  // graph from v to vars NOT in the root separator (i.e., to vars in the
  // residual graph that the root separator disconnects). High score
  // means v participates in many cross-residual-component clauses.
  // Normalized to [0, 100].
  centrality_score.assign(n_vars + 1, 0.0);
  if (!separator.empty()) {
    // Build the root separator var set.
    std::vector<bool> is_root_sep(n_vars + 1, false);
    unsigned n_root_sep = 0;
    for (const auto &cn : separator[root()]) {
      if (cn.kind == CutNode::VAR
          && cn.id >= 1 && cn.id <= (unsigned)n_vars) {
        is_root_sep[cn.id] = true;
        n_root_sep++;
      }
    }
    // Compute weighted degree for each root sep var: count edges from
    // v's primal-graph neighbors that are NOT in is_root_sep. Multi-edges
    // in full_adj naturally weight by clause count.
    double max_deg = 0.0;
    unsigned n_scored = 0;
    for (int v = 1; v <= n_vars; v++) {
      if (!is_root_sep[v]) continue;
      int gidx = v - 1;
      if (gidx < 0 || gidx >= (int)full_adj.size()) continue;
      double deg = 0.0;
      for (int u_gidx : full_adj[gidx]) {
        // Edges to clause-aux nodes (bipartite mode, gidx >= n_vars)
        // count toward "reach" too; vars-only mode has no aux nodes.
        if (u_gidx >= n_vars) {
          deg += 1.0;
          continue;
        }
        int u_var = u_gidx + 1;
        if (!is_root_sep[u_var]) deg += 1.0;
      }
      centrality_score[v] = deg;
      if (deg > max_deg) max_deg = deg;
      n_scored++;
    }
    if (max_deg > 0.0) {
      for (size_t v = 1; v < centrality_score.size(); v++) {
        centrality_score[v] = centrality_score[v] * 100.0 / max_deg;
      }
    }
    fprintf(stderr, "NDCentrality: root_sep_vars=%u scored=%u max_deg=%.1f\n",
            n_root_sep, n_scored, max_deg);
  }
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
  // Return convention:
  //   >= 0 : valid child to descend to.
  //   -1   : legitimate "can't descend" — parent is a leaf (no children)
  //          OR active_var_ids contributes no information (all vars have
  //          leaf < 0, or the list is empty). Not an error: the caller
  //          should just treat the hierarchy as unavailable for the
  //          sub-component from here on.
  //   -2   : INVARIANT VIOLATION — the sub-component's active vars span
  //          both children's subtrees. This must not happen when clause
  //          learning is correctly disabled during separator branching
  //          (BCP + clause removal cannot create cross-subtree edges).
  //          Callers should abort on -2 so the underlying bug is found.
  if (!valid || parent_node < 0 || parent_node >= n_nodes) return -1;

  int lc = left_child[parent_node];
  int rc = right_child[parent_node];
  if (lc < 0 || rc < 0) return -1;  // parent is a leaf — legitimate

  int left_lo_val = leaf_lo[lc], left_hi_val = leaf_hi[lc];
  if (left_lo_val < 0) return -1;

  int n_left = 0, n_right = 0;
  for (unsigned var_id : active_var_ids) {
    if (var_id >= var_leaf.size()) continue;
    int leaf = var_leaf[var_id];
    if (leaf < 0) continue;
    if (leaf >= left_lo_val && leaf <= left_hi_val)
      n_left++;
    else
      n_right++;
  }

  if (n_left == 0 && n_right == 0) return -1;  // empty info — legitimate
  if (n_left > 0 && n_right == 0) return lc;
  if (n_right > 0 && n_left == 0) return rc;
  return -2;  // mixed across subtrees — invariant violation
}

// ---------------------------------------------------------------
// Runtime METIS separator (one-shot) on a sub-component snapshot.
// See declaration in nd_hierarchy.h for semantics.
// ---------------------------------------------------------------
// File-scope state for the diagnostic dump (set via setReactiveMetisDumpPath).
static FILE     *g_react_metis_dump_fp = nullptr;
static long long g_react_metis_dump_id = 0;

void setReactiveMetisDumpPath(const std::string &path) {
  if (g_react_metis_dump_fp) {
    std::fclose(g_react_metis_dump_fp);
    g_react_metis_dump_fp = nullptr;
  }
  g_react_metis_dump_id = 0;
  if (!path.empty()) {
    g_react_metis_dump_fp = std::fopen(path.c_str(), "w");
    if (!g_react_metis_dump_fp) {
      std::cerr << "setReactiveMetisDumpPath: cannot open " << path << "\n";
    }
  }
}

RuntimeSeparatorResult computeRuntimeMetisSeparator(
    const vector<unsigned> &active_vars,
    const vector<pair<unsigned, vector<unsigned>>> &long_clauses,
    const vector<pair<unsigned, unsigned>> &binary_pairs)
{
  // Dump input record before doing anything else (so even early-return
  // cases are captured for replay).
  if (g_react_metis_dump_fp) {
    g_react_metis_dump_id++;
    std::fprintf(g_react_metis_dump_fp, "=R %lld %zu %zu %zu\n",
                 g_react_metis_dump_id, active_vars.size(),
                 long_clauses.size(), binary_pairs.size());
    std::fprintf(g_react_metis_dump_fp, "=V");
    for (auto v : active_vars) std::fprintf(g_react_metis_dump_fp, " %u", v);
    std::fprintf(g_react_metis_dump_fp, "\n");
    for (const auto &cl : long_clauses) {
      std::fprintf(g_react_metis_dump_fp, "=L %u", cl.first);
      for (auto v : cl.second) std::fprintf(g_react_metis_dump_fp, " %u", v);
      std::fprintf(g_react_metis_dump_fp, "\n");
    }
    for (const auto &p : binary_pairs)
      std::fprintf(g_react_metis_dump_fp, "=B %u %u\n", p.first, p.second);
    std::fprintf(g_react_metis_dump_fp, "=E\n");
  }

  RuntimeSeparatorResult out;
  const size_t n_vars = active_vars.size();
  const size_t n_cls  = long_clauses.size();
  const size_t n      = n_vars + n_cls;
  if (n < 4) return out;  // too small for METIS to bisect usefully

  auto t0 = chrono::steady_clock::now();

  // Map variable id → local 0..n_vars-1 index.
  unordered_map<unsigned, int> var_to_local;
  var_to_local.reserve(n_vars * 2);
  for (size_t i = 0; i < n_vars; i++) var_to_local[active_vars[i]] = (int)i;

  // Build adjacency lists in local-index space.
  vector<vector<int>> adj(n);
  // long clauses: bipartite var ↔ clause edges
  for (size_t ci = 0; ci < n_cls; ci++) {
    int cl_local = (int)(n_vars + ci);
    for (unsigned v : long_clauses[ci].second) {
      auto it = var_to_local.find(v);
      if (it == var_to_local.end()) continue;
      adj[it->second].push_back(cl_local);
      adj[cl_local].push_back(it->second);
    }
  }
  // binary clauses: direct var-var edges
  for (const auto &pr : binary_pairs) {
    auto ia = var_to_local.find(pr.first);
    auto ib = var_to_local.find(pr.second);
    if (ia == var_to_local.end() || ib == var_to_local.end()) continue;
    adj[ia->second].push_back(ib->second);
    adj[ib->second].push_back(ia->second);
  }

  // Build CSR for METIS.
  vector<idx_t> xadj(n + 1);
  vector<idx_t> adjncy;
  adjncy.reserve(2 * (n_cls * 3 + binary_pairs.size()));
  xadj[0] = 0;
  for (size_t i = 0; i < n; i++) {
    for (int u : adj[i]) adjncy.push_back(u);
    xadj[i + 1] = (idx_t)adjncy.size();
  }

  vector<idx_t> vwgt(n, 1);
  idx_t options[METIS_NOPTIONS];
  METIS_SetDefaultOptions(options);

  idx_t nvtxs = (idx_t)n;
  idx_t sepsize = 0;
  vector<idx_t> part(n);

  int ret = METIS_ComputeVertexSeparator(
      &nvtxs, xadj.data(), adjncy.data(),
      vwgt.data(), options, &sepsize, part.data());

  auto t1 = chrono::steady_clock::now();
  out.metis_elapsed_us =
      chrono::duration<double, std::micro>(t1 - t0).count();

  if (ret != METIS_OK) return out;

  // Translate local separator indices back to CutNodes.
  for (size_t i = 0; i < n; i++) {
    if (part[i] == 0) {
      if (i < n_vars) out.left_vars++;
    } else if (part[i] == 1) {
      if (i < n_vars) out.right_vars++;
    } else {
      // Separator vertex.
      if (i < n_vars) {
        out.separator.push_back(CutNode(CutNode::VAR, active_vars[i]));
      } else {
        size_t ci = i - n_vars;
        out.separator.push_back(CutNode(CutNode::CLAUSE,
                                         long_clauses[ci].first));
      }
    }
  }

  out.ok = (!out.separator.empty()
            && out.left_vars > 0
            && out.right_vars > 0);
  return out;
}
