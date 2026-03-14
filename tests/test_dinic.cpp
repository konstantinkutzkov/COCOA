/*
 * test_dinic.cpp
 *
 * Tests for Dinic's maxflow algorithm.
 */

#include "../src/dinic.h"
#include <iostream>
#include <cassert>

using namespace std;

// Test 1: Simple 2-node graph
void test_simple() {
  Dinic d(2);
  d.add_edge(0, 1, 5);
  assert(d.max_flow(0, 1) == 5);
  cout << "  test_simple: PASS" << endl;
}

// Test 2: Two parallel paths s->a->t, s->b->t with capacities 3 and 4
void test_parallel_paths() {
  // 0=s, 1=a, 2=b, 3=t
  Dinic d(4);
  d.add_edge(0, 1, 3);
  d.add_edge(1, 3, 3);
  d.add_edge(0, 2, 4);
  d.add_edge(2, 3, 4);
  assert(d.max_flow(0, 3) == 7);
  cout << "  test_parallel_paths: PASS" << endl;
}

// Test 3: Diamond graph with bottleneck
//   s -> a (10), s -> b (10), a -> t (5), b -> t (8), a -> b (3)
void test_diamond() {
  // 0=s, 1=a, 2=b, 3=t
  Dinic d(4);
  d.add_edge(0, 1, 10);
  d.add_edge(0, 2, 10);
  d.add_edge(1, 3, 5);
  d.add_edge(2, 3, 8);
  d.add_edge(1, 2, 3);
  assert(d.max_flow(0, 3) == 13);
  cout << "  test_diamond: PASS" << endl;
}

// Test 4: flow_cap parameter limits the flow
void test_flow_cap() {
  Dinic d(2);
  d.add_edge(0, 1, 100);
  assert(d.max_flow(0, 1, 10) >= 10);  // should stop at or above cap
  cout << "  test_flow_cap: PASS" << endl;
}

// Test 5: No path from s to t
void test_no_path() {
  Dinic d(3);
  d.add_edge(0, 1, 5);
  // no edge from 1 to 2
  assert(d.max_flow(0, 2) == 0);
  cout << "  test_no_path: PASS" << endl;
}

// Test 6: reachable_from after maxflow
void test_reachable() {
  // s(0) -> a(1) cap 1, a(1) -> t(2) cap 1
  // After maxflow=1, the edge 0->1 is saturated
  // Reachable from 0 should be just {0}
  Dinic d(3);
  d.add_edge(0, 1, 1);
  d.add_edge(1, 2, 1);
  int flow = d.max_flow(0, 2);
  assert(flow == 1);
  auto reach = d.reachable_from(0);
  assert(reach[0] == true);
  assert(reach[1] == false);
  assert(reach[2] == false);
  cout << "  test_reachable: PASS" << endl;
}

// Test 7: Node splitting for vertex cut
// Graph: s -- a -- b -- t (path of 4 nodes)
// Split each internal node: a_in -> a_out (cap 1), b_in -> b_out (cap 1)
// Edges: s_out -> a_in (INF), a_out -> b_in (INF), b_out -> t_in (INF)
// s and t have INF internal capacity
// Min vertex cut should be 1 (cut either a or b)
void test_vertex_cut_via_splitting() {
  // Nodes: s=0, a=1, b=2, t=3
  // Split: s_in=0, s_out=1, a_in=2, a_out=3, b_in=4, b_out=5, t_in=6, t_out=7
  Dinic d(8);
  // Internal edges
  d.add_edge(0, 1, INF_CAP);  // s_in -> s_out
  d.add_edge(2, 3, 1);        // a_in -> a_out (cap 1)
  d.add_edge(4, 5, 1);        // b_in -> b_out (cap 1)
  d.add_edge(6, 7, INF_CAP);  // t_in -> t_out

  // External edges (undirected, so both directions)
  d.add_edge(1, 2, INF_CAP);  // s_out -> a_in
  d.add_edge(3, 0, INF_CAP);  // a_out -> s_in
  d.add_edge(3, 4, INF_CAP);  // a_out -> b_in
  d.add_edge(5, 2, INF_CAP);  // b_out -> a_in
  d.add_edge(5, 6, INF_CAP);  // b_out -> t_in
  d.add_edge(7, 4, INF_CAP);  // t_out -> b_in

  int flow = d.max_flow(1, 6);  // s_out to t_in
  assert(flow == 1);

  // Find which node is in the cut
  auto reach = d.reachable_from(1);
  int cut_count = 0;
  // Check a: a_in reachable, a_out not reachable => in cut
  if (reach[2] && !reach[3]) cut_count++;
  // Check b: b_in reachable, b_out not reachable => in cut
  if (reach[4] && !reach[5]) cut_count++;

  assert(cut_count == 1);
  cout << "  test_vertex_cut_via_splitting: PASS" << endl;
}

int main() {
  cout << "Running Dinic tests..." << endl;
  test_simple();
  test_parallel_paths();
  test_diamond();
  test_flow_cap();
  test_no_path();
  test_reachable();
  test_vertex_cut_via_splitting();
  cout << "All Dinic tests passed!" << endl;
  return 0;
}
