/*
 * separator.h
 *
 * Minimum vertex cut (node separator) in the bipartite incidence graph.
 *
 * Implements s-t minimum vertex cut via:
 * - node splitting (x_in -> x_out with capacity cap(x))
 * - infinite-capacity incidence arcs
 * - Dinic maxflow / mincut
 */

#ifndef SEPARATOR_H_
#define SEPARATOR_H_

#include <vector>
#include <set>
#include <queue>
#include <algorithm>
#include <cassert>
#include <unordered_map>

#include "dinic.h"

// A node in the incidence graph: either a variable or a clause.
struct CutNode {
  enum Kind { VAR, CLAUSE };
  Kind kind;
  unsigned id;

  CutNode() : kind(VAR), id(0) {}
  CutNode(Kind k, unsigned i) : kind(k), id(i) {}

  bool operator==(const CutNode &o) const { return kind == o.kind && id == o.id; }
  bool operator!=(const CutNode &o) const { return !(*this == o); }
  bool operator<(const CutNode &o) const {
    if (kind != o.kind) return kind < o.kind;
    return id < o.id;
  }
};

struct SeparatorCandidate {
  std::vector<CutNode> separator;
  int flow_value;
  CutNode s, t;
  std::vector<int> component_var_sizes;  // descending
  int tries_used;
};

// Information needed by the separator finder about the current component.
// This is an abstraction so separator.h doesn't depend on instance.h directly.
// All index maps are precomputed at construction time.
struct FormulaInfo {
  // active (unassigned) variable IDs
  std::vector<unsigned> active_vars;

  // for each active clause: clause offset (used as clause ID)
  std::vector<unsigned> active_clause_ids;

  // clause_variables[clause_idx] = list of active variable IDs in that clause
  // (parallel to active_clause_ids)
  std::vector<std::vector<unsigned>> clause_variables;

  // var_to_clause_indices[var_idx] = list of clause indices (into active_clause_ids)
  // that contain this variable.
  // (parallel to active_vars)
  std::vector<std::vector<int>> var_to_clause_indices;

  // Precomputed ID -> index maps
  std::unordered_map<unsigned, int> var_id_to_idx;
  std::unordered_map<unsigned, int> cls_id_to_idx;

  // Call after populating active_vars, active_clause_ids, clause_variables.
  // Builds var_to_clause_indices and the ID -> index maps.
  void buildIndex();
};

// ---- Terminal selection via two-sweep BFS ----

// BFS on the incidence graph (bipartite: vars <-> clauses).
// Returns (farthest_var_index, distance).
std::pair<int, int> bfs_farthest_variable(
    const FormulaInfo &info,
    int start_var_idx,
    const std::vector<bool> &blocked_var,
    const std::vector<bool> &blocked_clause);

// Pick two far-apart terminals using two-sweep BFS diameter approximation.
std::pair<CutNode, CutNode> pick_terminals_two_sweep(
    const FormulaInfo &info,
    int rng_seed = 0);

// ---- Mincut separator computation ----

struct MinCutResult {
  int flow_value;
  std::vector<CutNode> separator;
};

MinCutResult compute_mincut_separator(
    const FormulaInfo &info,
    const CutNode &s,
    const CutNode &t,
    int flow_cap = 12);

// ---- Component analysis after removing separator ----

// After removing a set of nodes, find connected components.
// Returns list of components, each a list of CutNodes.
std::vector<std::vector<CutNode>> components_after_removing(
    const FormulaInfo &info,
    const std::set<CutNode> &removed);

// Count variables in a component.
int component_variable_count(const std::vector<CutNode> &comp);

// Verify a separator candidate: returns component var sizes (descending)
// or empty vector if invalid.
std::vector<int> verify_separator(
    const FormulaInfo &info,
    const std::vector<CutNode> &candidate,
    int min_second_component_vars = 5);

// ---- Main entry point ----

// Find the best separator by probing multiple terminal pairs.
// Returns true and fills result if a good separator is found.
bool find_best_separator(
    const FormulaInfo &info,
    SeparatorCandidate &result,
    int tries = 12,
    int seed = 0,
    int min_second_component_vars = 12,
    int max_separator_size = 12,
    int early_stop_size = 2,
    int flow_cap_nodes = 12);

#endif /* SEPARATOR_H_ */
