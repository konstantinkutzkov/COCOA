/*
 * dinic.h
 *
 * Dinic's algorithm for maximum flow.
 * Used as a building block for minimum vertex cut in the incidence graph.
 */

#ifndef DINIC_H_
#define DINIC_H_

#include <vector>
#include <queue>
#include <algorithm>
#include <cassert>

static const int INF_CAP = 1000000000;

class Dinic {
public:
  struct Edge {
    int to;
    int rev;  // index of reverse edge in g[to]
    int cap;
  };

  explicit Dinic(int n) : n_(n), g_(n), level_(n, -1), iter_(n, 0) {}

  void add_edge(int from, int to, int cap) {
    g_[from].push_back({to, (int)g_[to].size(), cap});
    g_[to].push_back({from, (int)g_[from].size() - 1, 0});
  }

  int max_flow(int s, int t, int flow_cap = INF_CAP) {
    int flow = 0;
    while (bfs(s, t)) {
      iter_.assign(n_, 0);
      while (true) {
        int pushed = dfs(s, t, INF_CAP);
        if (pushed == 0) break;
        flow += pushed;
        if (flow >= flow_cap) return flow;
      }
    }
    return flow;
  }

  // After max_flow, returns which nodes are reachable from s in the residual graph.
  std::vector<bool> reachable_from(int s) const {
    std::vector<bool> seen(n_, false);
    std::queue<int> q;
    seen[s] = true;
    q.push(s);
    while (!q.empty()) {
      int v = q.front(); q.pop();
      for (const auto &e : g_[v]) {
        if (e.cap > 0 && !seen[e.to]) {
          seen[e.to] = true;
          q.push(e.to);
        }
      }
    }
    return seen;
  }

  int num_nodes() const { return n_; }

private:
  bool bfs(int s, int t) {
    level_.assign(n_, -1);
    std::queue<int> q;
    level_[s] = 0;
    q.push(s);
    while (!q.empty()) {
      int v = q.front(); q.pop();
      for (const auto &e : g_[v]) {
        if (e.cap > 0 && level_[e.to] < 0) {
          level_[e.to] = level_[v] + 1;
          q.push(e.to);
        }
      }
    }
    return level_[t] >= 0;
  }

  int dfs(int v, int t, int f) {
    if (v == t) return f;
    for (int &i = iter_[v]; i < (int)g_[v].size(); i++) {
      Edge &e = g_[v][i];
      if (e.cap > 0 && level_[e.to] == level_[v] + 1) {
        int ret = dfs(e.to, t, std::min(f, e.cap));
        if (ret > 0) {
          e.cap -= ret;
          g_[e.to][e.rev].cap += ret;
          return ret;
        }
      }
    }
    return 0;
  }

  int n_;
  std::vector<std::vector<Edge>> g_;
  std::vector<int> level_;
  std::vector<int> iter_;
};

#endif /* DINIC_H_ */
