/*
 * test_separator.cpp
 *
 * Tests for separator finding on small formulas.
 */

#include "../src/separator.h"
#include <iostream>
#include <cassert>
#include <set>

using namespace std;

// Helper: build FormulaInfo from a list of clauses.
// vars are 1-based, clause IDs are 0-based offsets.
FormulaInfo make_formula(int n_vars, const vector<vector<int>> &clauses) {
  FormulaInfo info;

  // Find active vars (those appearing in at least one clause)
  set<unsigned> var_set;
  for (const auto &cl : clauses)
    for (int lit : cl)
      var_set.insert(abs(lit));

  info.active_vars.assign(var_set.begin(), var_set.end());

  for (int i = 0; i < (int)clauses.size(); i++) {
    info.active_clause_ids.push_back(i);
    vector<unsigned> vars_in_clause;
    for (int lit : clauses[i])
      vars_in_clause.push_back(abs(lit));
    info.clause_variables.push_back(vars_in_clause);
  }

  info.buildIndex();
  return info;
}


// Test 1: Path-like formula
// Two groups of variables connected by a single bridge variable.
// Group A: vars {1,2,3} with clauses among them
// Bridge: var 4
// Group B: vars {5,6,7} with clauses among them
// Clauses connecting A to bridge: (3, 4)
// Clauses connecting bridge to B: (4, 5)
// Separator should be {var 4}
void test_bridge_variable() {
  vector<vector<int>> clauses = {
    {1, 2},    // clause 0: in group A
    {2, 3},    // clause 1: in group A
    {1, 3},    // clause 2: in group A
    {3, 4},    // clause 3: bridge A-to-4
    {4, 5},    // clause 4: bridge 4-to-B
    {5, 6},    // clause 5: in group B
    {6, 7},    // clause 6: in group B
    {5, 7},    // clause 7: in group B
  };

  FormulaInfo info = make_formula(7, clauses);

  SeparatorCandidate result;
  bool found = find_best_separator(info, result,
    /*tries=*/12, /*seed=*/0,
    /*min_second_component_vars=*/2,
    /*max_separator_size=*/5,
    /*early_stop_size=*/2,
    /*flow_cap_nodes=*/12);

  assert(found);
  cout << "  Separator size: " << result.separator.size() << endl;
  cout << "  Separator: ";
  for (const auto &nd : result.separator) {
    cout << (nd.kind == CutNode::VAR ? "var" : "clause") << " " << nd.id << ", ";
  }
  cout << endl;
  cout << "  Component sizes: ";
  for (int s : result.component_var_sizes)
    cout << s << " ";
  cout << endl;

  // The separator should have size 1 and it should be var 4
  // (or possibly one of the bridge clauses)
  assert(result.separator.size() <= 2);

  // Verify the separator actually disconnects the graph
  set<CutNode> removed(result.separator.begin(), result.separator.end());
  auto comps = components_after_removing(info, removed);
  int num_var_comps = 0;
  for (const auto &c : comps)
    if (component_variable_count(c) > 0) num_var_comps++;
  assert(num_var_comps >= 2);

  cout << "  test_bridge_variable: PASS" << endl;
}


// Test 2: Two cliques connected by a bridge clause
// Group A: vars {1,2,3,4} fully connected
// Group B: vars {5,6,7,8} fully connected
// One clause connects them: {4, 5}
void test_bridge_clause() {
  vector<vector<int>> clauses = {
    // Group A clique
    {1, 2}, {1, 3}, {1, 4}, {2, 3}, {2, 4}, {3, 4},
    // Group B clique
    {5, 6}, {5, 7}, {5, 8}, {6, 7}, {6, 8}, {7, 8},
    // Bridge
    {4, 5},
  };

  FormulaInfo info = make_formula(8, clauses);

  SeparatorCandidate result;
  bool found = find_best_separator(info, result,
    /*tries=*/12, /*seed=*/0,
    /*min_second_component_vars=*/2,
    /*max_separator_size=*/5,
    /*early_stop_size=*/2,
    /*flow_cap_nodes=*/12);

  assert(found);
  cout << "  Separator size: " << result.separator.size() << endl;
  cout << "  Separator: ";
  for (const auto &nd : result.separator) {
    cout << (nd.kind == CutNode::VAR ? "var" : "clause") << " " << nd.id << ", ";
  }
  cout << endl;

  // Should find a small separator (size 1 or 2)
  assert(result.separator.size() <= 3);

  cout << "  test_bridge_clause: PASS" << endl;
}


// Test 3: Disconnected formula (two independent components)
// No separator needed — formula is already disconnected.
void test_disconnected() {
  vector<vector<int>> clauses = {
    {1, 2}, {2, 3},       // component 1
    {4, 5}, {5, 6},       // component 2
  };

  FormulaInfo info = make_formula(6, clauses);

  // The two-sweep BFS will stay in one component,
  // so terminals will be in the same component.
  // The mincut should fail to find a useful separator
  // (both components already separate).
  SeparatorCandidate result;
  bool found = find_best_separator(info, result,
    /*tries=*/4, /*seed=*/0,
    /*min_second_component_vars=*/2,
    /*max_separator_size=*/5);

  // It's OK if we find something or not — the formula is already disconnected.
  // But if found, the separator should be valid.
  if (found) {
    set<CutNode> removed(result.separator.begin(), result.separator.end());
    auto comps = components_after_removing(info, removed);
    int num_var_comps = 0;
    for (const auto &c : comps)
      if (component_variable_count(c) > 0) num_var_comps++;
    assert(num_var_comps >= 2);
  }

  cout << "  test_disconnected: PASS" << endl;
}


// Test 4: verify_separator function
void test_verify_separator() {
  vector<vector<int>> clauses = {
    {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6},
  };
  FormulaInfo info = make_formula(6, clauses);

  // Removing var 3 should split into {1,2} and {4,5,6}
  vector<CutNode> sep = {CutNode(CutNode::VAR, 3)};
  auto sizes = verify_separator(info, sep, /*min_second=*/2);
  assert(!sizes.empty());
  assert(sizes[0] == 3);  // {4,5,6}
  assert(sizes[1] == 2);  // {1,2}

  // Removing var 1 should not be a valid separator (only one big component)
  vector<CutNode> bad_sep = {CutNode(CutNode::VAR, 1)};
  auto sizes2 = verify_separator(info, bad_sep, /*min_second=*/2);
  // var 1 is at the end, removing it leaves {2,3,4,5,6} as one component
  // and nothing else with >= 2 vars
  assert(sizes2.empty());

  cout << "  test_verify_separator: PASS" << endl;
}


// Test 5: components_after_removing
void test_components_after_removing() {
  vector<vector<int>> clauses = {
    {1, 2}, {2, 3}, {3, 4}, {4, 5},
  };
  FormulaInfo info = make_formula(5, clauses);

  // Remove var 3 -> should split into two components
  set<CutNode> removed = {CutNode(CutNode::VAR, 3)};
  auto comps = components_after_removing(info, removed);

  int total_vars = 0;
  for (const auto &c : comps)
    total_vars += component_variable_count(c);

  assert(total_vars == 4);  // 5 vars - 1 removed = 4

  int num_var_comps = 0;
  for (const auto &c : comps)
    if (component_variable_count(c) > 0) num_var_comps++;
  assert(num_var_comps == 2);

  cout << "  test_components_after_removing: PASS" << endl;
}


int main() {
  cout << "Running separator tests..." << endl;
  test_bridge_variable();
  test_bridge_clause();
  test_disconnected();
  test_verify_separator();
  test_components_after_removing();
  cout << "All separator tests passed!" << endl;
  return 0;
}
