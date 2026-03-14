/*
 * separator.cpp
 *
 * Implementation of minimum vertex cut separator finding.
 */

#include "separator.h"

#include <unordered_map>
#include <cmath>

using namespace std;


// ---------- FormulaInfo index building ----------

void FormulaInfo::buildIndex() {
  int n_vars = active_vars.size();
  int n_cls = active_clause_ids.size();

  // Build ID -> index maps
  var_id_to_idx.clear();
  var_id_to_idx.reserve(n_vars);
  for (int i = 0; i < n_vars; i++)
    var_id_to_idx[active_vars[i]] = i;

  cls_id_to_idx.clear();
  cls_id_to_idx.reserve(n_cls);
  for (int i = 0; i < n_cls; i++)
    cls_id_to_idx[active_clause_ids[i]] = i;

  // Build reverse index: var_idx -> list of clause indices
  var_to_clause_indices.assign(n_vars, {});
  for (int ci = 0; ci < n_cls; ci++) {
    for (unsigned var_id : clause_variables[ci]) {
      auto it = var_id_to_idx.find(var_id);
      if (it != var_id_to_idx.end()) {
        var_to_clause_indices[it->second].push_back(ci);
      }
    }
  }
}


// ---------- BFS on incidence graph ----------

// BFS in the incidence graph (vars <-> clauses).
// Nodes are identified by type + index into active_vars / active_clause_ids.
// Returns (farthest_var_index, distance).
pair<int, int> bfs_farthest_variable(
    const FormulaInfo &info,
    int start_var_idx,
    const vector<bool> &blocked_var,
    const vector<bool> &blocked_clause)
{
  int n_vars = info.active_vars.size();
  int n_cls = info.active_clause_ids.size();

  vector<int> dist_var(n_vars, -1);
  vector<int> dist_cls(n_cls, -1);

  queue<pair<bool, int>> q;
  dist_var[start_var_idx] = 0;
  q.push({false, start_var_idx});

  int far_var_idx = start_var_idx;
  int far_dist = 0;

  while (!q.empty()) {
    bool is_clause = q.front().first;
    int idx = q.front().second;
    q.pop();

    if (is_clause) {
      int d = dist_cls[idx];
      // Clause -> expand to its variables (using precomputed var_id_to_idx)
      for (unsigned var_id : info.clause_variables[idx]) {
        auto it = info.var_id_to_idx.find(var_id);
        if (it == info.var_id_to_idx.end()) continue;
        int vi = it->second;
        if (blocked_var[vi] || dist_var[vi] >= 0) continue;
        dist_var[vi] = d + 1;
        if (dist_var[vi] > far_dist) {
          far_dist = dist_var[vi];
          far_var_idx = vi;
        }
        q.push(make_pair(false, vi));
      }
    } else {
      int d = dist_var[idx];
      // Variable -> expand to its clauses (using precomputed var_to_clause_indices)
      for (int ci : info.var_to_clause_indices[idx]) {
        if (blocked_clause[ci] || dist_cls[ci] >= 0) continue;
        dist_cls[ci] = d + 1;
        q.push(make_pair(true, ci));
      }
    }
  }

  return {far_var_idx, far_dist};
}


pair<CutNode, CutNode> pick_terminals_two_sweep(
    const FormulaInfo &info,
    int rng_seed)
{
  int n_vars = info.active_vars.size();
  assert(n_vars > 0);

  vector<bool> no_block_var(n_vars, false);
  vector<bool> no_block_cls(info.active_clause_ids.size(), false);

  // Deterministic "random" pick
  unsigned idx = ((unsigned)rng_seed * 2654435761u) % (unsigned)n_vars;

  pair<int,int> sweep1 = bfs_farthest_variable(info, idx, no_block_var, no_block_cls);
  int s_idx = sweep1.first;
  pair<int,int> sweep2 = bfs_farthest_variable(info, s_idx, no_block_var, no_block_cls);
  int t_idx = sweep2.first;

  return {
    CutNode(CutNode::VAR, info.active_vars[s_idx]),
    CutNode(CutNode::VAR, info.active_vars[t_idx])
  };
}


// ---------- Mincut computation ----------

MinCutResult compute_mincut_separator(
    const FormulaInfo &info,
    const CutNode &s,
    const CutNode &t,
    int flow_cap)
{
  int n_vars = info.active_vars.size();
  int n_cls = info.active_clause_ids.size();
  int n_nodes = n_vars + n_cls;

  // Node index: vars are 0..n_vars-1, clauses are n_vars..n_nodes-1
  auto node_index = [&](const CutNode &nd) -> int {
    if (nd.kind == CutNode::VAR)
      return info.var_id_to_idx.at(nd.id);
    return n_vars + info.cls_id_to_idx.at(nd.id);
  };

  // Split graph: node i has i_in = 2*i, i_out = 2*i+1
  auto n_in = [](int i) { return 2 * i; };
  auto n_out = [](int i) { return 2 * i + 1; };

  Dinic dinic(2 * n_nodes);

  int s_i = node_index(s);
  int t_i = node_index(t);

  // Add split edges with unit capacity (except terminals)
  for (int i = 0; i < n_vars; i++) {
    int cap = 1;
    if (i == s_i && s.kind == CutNode::VAR) cap = INF_CAP;
    if (i == t_i && t.kind == CutNode::VAR) cap = INF_CAP;
    dinic.add_edge(n_in(i), n_out(i), cap);
  }

  for (int j = 0; j < n_cls; j++) {
    int node_j = n_vars + j;
    int cap = 1;
    if (node_j == s_i && s.kind == CutNode::CLAUSE) cap = INF_CAP;
    if (node_j == t_i && t.kind == CutNode::CLAUSE) cap = INF_CAP;
    dinic.add_edge(n_in(node_j), n_out(node_j), cap);
  }

  // Add incidence arcs: var <-> clause with INF capacity
  for (int ci = 0; ci < n_cls; ci++) {
    int c_node = n_vars + ci;
    for (unsigned var_id : info.clause_variables[ci]) {
      auto it = info.var_id_to_idx.find(var_id);
      if (it == info.var_id_to_idx.end()) continue;
      int v_node = it->second;
      dinic.add_edge(n_out(v_node), n_in(c_node), INF_CAP);
      dinic.add_edge(n_out(c_node), n_in(v_node), INF_CAP);
    }
  }

  int source = n_out(s_i);
  int sink = n_in(t_i);

  int flow = dinic.max_flow(source, sink, flow_cap);
  auto reach = dinic.reachable_from(source);

  // Extract separator: nodes where x_in is reachable but x_out is not
  vector<CutNode> separator;
  for (int i = 0; i < n_vars; i++) {
    if (reach[n_in(i)] && !reach[n_out(i)]) {
      separator.push_back(CutNode(CutNode::VAR, info.active_vars[i]));
    }
  }
  for (int j = 0; j < n_cls; j++) {
    int node_j = n_vars + j;
    if (reach[n_in(node_j)] && !reach[n_out(node_j)]) {
      separator.push_back(CutNode(CutNode::CLAUSE, info.active_clause_ids[j]));
    }
  }

  return {flow, separator};
}


// ---------- Component analysis ----------

// TODO: Consider replacing set<CutNode> with vector<bool> indexed by var/clause
// for better performance on large instances.
vector<vector<CutNode>> components_after_removing(
    const FormulaInfo &info,
    const set<CutNode> &removed)
{
  int n_vars = info.active_vars.size();
  int n_cls = info.active_clause_ids.size();

  // Track which vars/clauses are removed
  vector<bool> var_removed(n_vars, false);
  vector<bool> cls_removed(n_cls, false);

  for (const auto &nd : removed) {
    if (nd.kind == CutNode::VAR) {
      auto it = info.var_id_to_idx.find(nd.id);
      if (it != info.var_id_to_idx.end())
        var_removed[it->second] = true;
    } else {
      auto it = info.cls_id_to_idx.find(nd.id);
      if (it != info.cls_id_to_idx.end())
        cls_removed[it->second] = true;
    }
  }

  // BFS to find connected components
  vector<bool> var_seen(n_vars, false);
  vector<bool> cls_seen(n_cls, false);

  vector<vector<CutNode>> comps;

  for (int start = 0; start < n_vars; start++) {
    if (var_removed[start] || var_seen[start]) continue;

    vector<CutNode> comp;
    queue<pair<bool, int>> q;  // (is_clause, index)
    var_seen[start] = true;
    q.push({false, start});

    while (!q.empty()) {
      bool is_clause = q.front().first;
      int idx = q.front().second;
      q.pop();

      if (is_clause) {
        comp.push_back(CutNode(CutNode::CLAUSE, info.active_clause_ids[idx]));
        // Expand to variables (using precomputed var_id_to_idx)
        for (unsigned var_id : info.clause_variables[idx]) {
          auto it = info.var_id_to_idx.find(var_id);
          if (it == info.var_id_to_idx.end()) continue;
          int vi = it->second;
          if (var_removed[vi] || var_seen[vi]) continue;
          var_seen[vi] = true;
          q.push({false, vi});
        }
      } else {
        comp.push_back(CutNode(CutNode::VAR, info.active_vars[idx]));
        // Expand to clauses (using precomputed var_to_clause_indices)
        for (int ci : info.var_to_clause_indices[idx]) {
          if (cls_removed[ci] || cls_seen[ci]) continue;
          cls_seen[ci] = true;
          q.push({true, ci});
        }
      }
    }

    if (!comp.empty())
      comps.push_back(comp);
  }

  // Also add orphaned clauses (clauses with no remaining active variables)
  for (int ci = 0; ci < n_cls; ci++) {
    if (cls_removed[ci] || cls_seen[ci]) continue;
    comps.push_back({CutNode(CutNode::CLAUSE, info.active_clause_ids[ci])});
  }

  return comps;
}


int component_variable_count(const vector<CutNode> &comp) {
  int count = 0;
  for (const auto &nd : comp)
    if (nd.kind == CutNode::VAR)
      count++;
  return count;
}


vector<int> verify_separator(
    const FormulaInfo &info,
    const vector<CutNode> &candidate,
    int min_second_component_vars)
{
  set<CutNode> removed(candidate.begin(), candidate.end());
  auto comps = components_after_removing(info, removed);

  vector<int> sizes;
  for (const auto &comp : comps)
    sizes.push_back(component_variable_count(comp));

  sort(sizes.begin(), sizes.end(), greater<int>());

  if (sizes.size() < 2 || sizes[1] < min_second_component_vars)
    return {};

  return sizes;
}


// ---------- Main entry point ----------

bool find_best_separator(
    const FormulaInfo &info,
    SeparatorCandidate &result,
    int tries,
    int seed,
    int min_second_component_vars,
    int max_separator_size,
    int early_stop_size,
    int flow_cap_nodes)
{
  int n_active = info.active_vars.size();
  if (n_active == 0 || info.active_clause_ids.empty())
    return false;

  bool found = false;
  // best_key: (separator_size, -second_comp_size, flow_value) — smaller is better
  int best_sep_size = max_separator_size + 1;
  int best_neg_second = 0;
  int best_flow = 0;

  for (int i = 0; i < max(1, tries); i++) {
    pair<CutNode, CutNode> terminals = pick_terminals_two_sweep(info, seed + i);
    CutNode s = terminals.first;
    CutNode t = terminals.second;

    auto res = compute_mincut_separator(info, s, t, flow_cap_nodes);

    if (res.separator.empty() || (int)res.separator.size() > max_separator_size)
      continue;

    set<CutNode> removed(res.separator.begin(), res.separator.end());
    auto comps = components_after_removing(info, removed);

    vector<int> sizes;
    for (const auto &comp : comps)
      sizes.push_back(component_variable_count(comp));
    sort(sizes.begin(), sizes.end(), greater<int>());

    if ((int)sizes.size() < 2 || sizes[1] < min_second_component_vars)
      continue;

    int sep_size = res.separator.size();
    int neg_second = -sizes[1];
    int flow_val = res.flow_value;

    // Compare: (sep_size, neg_second, flow_val) — lexicographic, smaller is better
    bool is_better = false;
    if (!found) {
      is_better = true;
    } else if (sep_size < best_sep_size) {
      is_better = true;
    } else if (sep_size == best_sep_size && neg_second < best_neg_second) {
      is_better = true;
    } else if (sep_size == best_sep_size && neg_second == best_neg_second && flow_val < best_flow) {
      is_better = true;
    }

    if (is_better) {
      best_sep_size = sep_size;
      best_neg_second = neg_second;
      best_flow = flow_val;
      result.separator = res.separator;
      result.flow_value = flow_val;
      result.s = s;
      result.t = t;
      result.component_var_sizes = sizes;
      result.tries_used = i + 1;
      found = true;

      // Early stop for small separators
      if (sep_size <= max(1, early_stop_size))
        break;
    }
  }

  return found;
}
