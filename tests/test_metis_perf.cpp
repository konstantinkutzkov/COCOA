/*
 * test_metis_perf.cpp
 *
 * Standalone benchmark of METIS_ComputeVertexSeparator throughput as a
 * function of subgraph size. No solver code linked.
 *
 * Given a CNF, build the bipartite incidence graph. For each target size
 * N, repeatedly sample a random connected subgraph of size N (BFS from a
 * random seed vertex), call METIS on it, and time the call. Report
 * min/mean/max per-call wall time and separator size.
 *
 * Intended to answer: does METIS have a large fixed per-call overhead
 * that makes it unattractive for small subgraphs?
 */

#include <metis.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <random>
#include <queue>
#include <chrono>
#include <algorithm>

using namespace std;
using Clock = chrono::steady_clock;

struct Graph {
  int n_vars = 0;
  int n_clauses = 0;
  // Bipartite: 0..n_vars-1 = variables, n_vars..n_vars+n_clauses-1 = clauses.
  vector<vector<int>> adj;

  int n_full() const { return n_vars + n_clauses; }
};

static Graph read_cnf(const string &path) {
  Graph g;
  ifstream f(path);
  if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); exit(1); }
  string line;
  vector<vector<int>> clauses;
  while (getline(f, line)) {
    if (line.empty() || line[0] == 'c') continue;
    if (line[0] == 'p') {
      istringstream s(line);
      string p, cnf;
      int nv, nc;
      s >> p >> cnf >> nv >> nc;
      g.n_vars = nv;
      clauses.reserve(nc);
      continue;
    }
    istringstream s(line);
    vector<int> cl;
    int lit;
    while (s >> lit) {
      if (lit == 0) break;
      int v = abs(lit);
      cl.push_back(v);
    }
    if (!cl.empty()) clauses.push_back(std::move(cl));
  }
  g.n_clauses = clauses.size();
  g.adj.assign(g.n_full(), {});
  for (int ci = 0; ci < g.n_clauses; ci++) {
    int cgidx = g.n_vars + ci;
    for (int v : clauses[ci]) {
      int vgidx = v - 1;  // 0-indexed
      if (vgidx < 0 || vgidx >= g.n_vars) continue;
      g.adj[vgidx].push_back(cgidx);
      g.adj[cgidx].push_back(vgidx);
    }
  }
  return g;
}

// Sample a random connected subgraph by BFS from a random seed.
// Returns full-graph vertex indices of size up to target_size.
static vector<int> sample_subgraph(const Graph &g, int target_size,
                                    mt19937 &rng) {
  int n = g.n_full();
  if (n == 0 || target_size <= 0) return {};
  vector<char> seen(n, 0);
  vector<int> out;
  out.reserve(target_size);
  // Pick a seed that has at least one neighbour
  for (int tries = 0; tries < 32; tries++) {
    int s = rng() % n;
    if (!g.adj[s].empty()) {
      queue<int> q;
      q.push(s);
      seen[s] = 1;
      while (!q.empty() && (int)out.size() < target_size) {
        int v = q.front(); q.pop();
        out.push_back(v);
        // Randomise neighbour order to get diverse subgraphs
        auto nbrs = g.adj[v];
        shuffle(nbrs.begin(), nbrs.end(), rng);
        for (int u : nbrs) {
          if (!seen[u]) {
            seen[u] = 1;
            q.push(u);
          }
        }
      }
      break;
    }
  }
  return out;
}

struct CallResult {
  double us;        // microseconds
  int sep_size;
  int n_vtx;
};

static CallResult run_metis_on(const Graph &g, const vector<int> &vertices) {
  int n = vertices.size();
  CallResult r{0.0, -1, n};
  if (n < 4) return r;

  // Map full → local
  vector<int> local_of(g.n_full(), -1);
  for (int i = 0; i < n; i++) local_of[vertices[i]] = i;

  vector<idx_t> xadj(n + 1, 0);
  vector<idx_t> adjncy;
  adjncy.reserve(4 * n);
  for (int i = 0; i < n; i++) {
    int v = vertices[i];
    for (int u : g.adj[v]) {
      int lu = local_of[u];
      if (lu >= 0) adjncy.push_back(lu);
    }
    xadj[i + 1] = adjncy.size();
  }

  // Reset local_of for reuse
  for (int v : vertices) local_of[v] = -1;

  vector<idx_t> vwgt(n, 1);
  idx_t options[METIS_NOPTIONS];
  METIS_SetDefaultOptions(options);

  idx_t nvtxs = n;
  idx_t sepsize = 0;
  vector<idx_t> part(n);

  auto t0 = Clock::now();
  int ret = METIS_ComputeVertexSeparator(
      &nvtxs, xadj.data(), adjncy.data(),
      vwgt.data(), options, &sepsize, part.data());
  auto t1 = Clock::now();

  r.us = chrono::duration<double, micro>(t1 - t0).count();
  if (ret == METIS_OK) r.sep_size = (int)sepsize;
  return r;
}

struct Stats {
  double min_us = 1e30, max_us = 0, sum_us = 0;
  int n = 0;
  double sep_sum = 0;
  int sep_min = INT32_MAX, sep_max = 0;
  void add(double us, int sep) {
    if (us < min_us) min_us = us;
    if (us > max_us) max_us = us;
    sum_us += us;
    n++;
    sep_sum += sep;
    if (sep < sep_min) sep_min = sep;
    if (sep > sep_max) sep_max = sep;
  }
  double mean_us() const { return n ? sum_us / n : 0; }
  double mean_sep() const { return n ? sep_sum / n : 0; }
};

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
      "usage: %s <cnf> [trials_per_size=20] [burst_n=200] [burst_size=200]\n",
      argv[0]);
    return 1;
  }
  const char *cnf = argv[1];
  int trials = (argc > 2) ? atoi(argv[2]) : 20;
  int burst_n = (argc > 3) ? atoi(argv[3]) : 200;
  int burst_size = (argc > 4) ? atoi(argv[4]) : 200;

  Graph g = read_cnf(cnf);
  int full = g.n_full();
  fprintf(stderr, "loaded %s: %d vars + %d clauses = %d full vertices\n",
          cnf, g.n_vars, g.n_clauses, full);

  vector<int> sizes = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000};
  // Remove sizes exceeding graph
  sizes.erase(remove_if(sizes.begin(), sizes.end(),
                        [&](int x){ return x > full; }),
              sizes.end());
  if (sizes.empty() || sizes.back() < full) sizes.push_back(full);

  mt19937 rng(42);

  printf("\n# Random-subgraph timing, %d trials per size\n", trials);
  printf("# size  trials  min_us   mean_us  max_us   calls/s   mean_sep  min_sep  max_sep\n");
  for (int N : sizes) {
    Stats s;
    int attempted = 0, success = 0;
    for (int t = 0; t < trials; t++) {
      attempted++;
      vector<int> sub = sample_subgraph(g, N, rng);
      if ((int)sub.size() < 4) continue;
      CallResult r = run_metis_on(g, sub);
      if (r.sep_size < 0) continue;
      success++;
      s.add(r.us, r.sep_size);
    }
    if (s.n == 0) {
      printf("%5d  %d/%d   (no successful calls)\n", N, success, attempted);
      continue;
    }
    double calls_per_s = 1e6 / s.mean_us();
    printf("%5d  %2d      %7.1f  %7.1f  %7.1f  %8.1f  %8.1f  %5d   %5d\n",
           N, s.n, s.min_us, s.mean_us(), s.max_us, calls_per_s,
           s.mean_sep(), s.sep_min, s.sep_max);
  }

  // Burst test: many back-to-back calls on subgraphs of the same size
  // to expose any warmup cost or caching effects
  printf("\n# Burst: %d back-to-back calls at size %d\n", burst_n, burst_size);
  printf("# idx    us    sep_size\n");
  Stats burst;
  vector<double> burst_us;
  burst_us.reserve(burst_n);
  for (int i = 0; i < burst_n; i++) {
    vector<int> sub = sample_subgraph(g, burst_size, rng);
    if ((int)sub.size() < 4) continue;
    CallResult r = run_metis_on(g, sub);
    if (r.sep_size < 0) continue;
    burst.add(r.us, r.sep_size);
    burst_us.push_back(r.us);
    if (i < 5 || i >= burst_n - 5) {
      printf("%5d  %7.1f  %3d\n", i, r.us, r.sep_size);
    } else if (i == 5) {
      printf("  ...\n");
    }
  }
  if (burst.n > 0) {
    // First-call / later-call comparison
    int head = min(5, burst.n);
    int tail = min(5, burst.n);
    double head_sum = 0, tail_sum = 0;
    for (int i = 0; i < head; i++) head_sum += burst_us[i];
    for (int i = (int)burst_us.size() - tail; i < (int)burst_us.size(); i++)
      tail_sum += burst_us[i];
    printf("\n# burst summary: n=%d  min=%.1fus  mean=%.1fus  max=%.1fus  calls/s=%.0f\n",
           burst.n, burst.min_us, burst.mean_us(), burst.max_us,
           1e6 / burst.mean_us());
    printf("# first-%d mean: %.1fus   last-%d mean: %.1fus\n",
           head, head_sum / head, tail, tail_sum / tail);
  }

  return 0;
}
