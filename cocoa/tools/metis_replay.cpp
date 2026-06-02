/*
 * tools/metis_replay.cpp
 *
 * Standalone replay tool: reads a dump file produced by sharpSAT's
 * -dumpReactiveMetisInputs flag (each record = one input passed to
 * computeRuntimeMetisSeparator), runs METIS_ComputeVertexSeparator on
 * each in isolation, classifies the outcome, and prints aggregate
 * statistics.
 *
 * Purpose: characterise WHY reactive METIS "fails" so often when
 * called from the live solver — distinguishing among:
 *   (1) input too small (n < 4)
 *   (2) METIS returned an error code
 *   (3) METIS returned but result was degenerate
 *       (3a) empty separator
 *       (3b) one-sided partition (left or right has 0 vars)
 *   (4) accepted but trivial (sep size <= 2)
 *   (5) accepted, non-trivial separator
 *
 * Per-input properties:
 *   - n_vars, n_long_clauses, n_binary_pairs, total_edges
 *   - connected components of the input graph (BFS)
 *   - separator size and partition balance (if METIS succeeded)
 *
 * Build: linked into the main CMake target list. See CMakeLists.txt.
 *
 * Input format (one record):
 *   =R <id> <n_vars> <n_long> <n_bin>
 *   =V <v1> <v2> ... <v_n_vars>
 *   =L <ofs> <v1> <v2> ... <v_k>      # one per long clause
 *   =B <a> <b>                         # one per binary
 *   =E
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <queue>
#include <algorithm>
#include <chrono>
#include <metis.h>


struct Record {
  long long id = 0;
  std::vector<unsigned> vars;
  std::vector<std::pair<unsigned, std::vector<unsigned>>> long_clauses;
  std::vector<std::pair<unsigned, unsigned>> binary_pairs;
};


static bool read_record(std::istream &in, Record &r) {
  // Read one record. Returns false at EOF or on parse error.
  r = Record();
  std::string line;
  bool got_R = false;
  size_t expected_long = 0, expected_bin = 0;
  while (std::getline(in, line)) {
    if (line.size() < 2 || line[0] != '=') continue;
    char tag = line[1];
    std::istringstream iss(line.substr(2));
    if (tag == 'R') {
      long long id;
      size_t n_vars, n_long, n_bin;
      if (!(iss >> id >> n_vars >> n_long >> n_bin)) return false;
      r.id = id;
      r.vars.reserve(n_vars);
      r.long_clauses.reserve(n_long);
      r.binary_pairs.reserve(n_bin);
      expected_long = n_long;
      expected_bin = n_bin;
      got_R = true;
    } else if (tag == 'V') {
      unsigned v;
      while (iss >> v) r.vars.push_back(v);
    } else if (tag == 'L') {
      unsigned ofs;
      if (!(iss >> ofs)) return false;
      std::vector<unsigned> members;
      unsigned v;
      while (iss >> v) members.push_back(v);
      r.long_clauses.push_back({ofs, std::move(members)});
    } else if (tag == 'B') {
      unsigned a, b;
      if (!(iss >> a >> b)) return false;
      r.binary_pairs.push_back({a, b});
    } else if (tag == 'E') {
      // Sanity check on counts.
      if (r.long_clauses.size() != expected_long
          || r.binary_pairs.size() != expected_bin) {
        std::cerr << "warn: record " << r.id
                  << " count mismatch (long: " << r.long_clauses.size()
                  << "/" << expected_long
                  << " bin: " << r.binary_pairs.size()
                  << "/" << expected_bin << ")\n";
      }
      return got_R;
    }
  }
  return false;
}


// Build the same adjacency list computeRuntimeMetisSeparator builds
// (so connected-components and edge counts match what METIS sees).
struct Graph {
  size_t n_vars;
  size_t n_cls;
  size_t n;          // n_vars + n_cls
  std::vector<std::vector<int>> adj;  // local-index space [0..n)
};


static Graph build_graph(const Record &r) {
  Graph g;
  g.n_vars = r.vars.size();
  g.n_cls  = r.long_clauses.size();
  g.n      = g.n_vars + g.n_cls;
  g.adj.assign(g.n, {});

  std::unordered_map<unsigned, int> var_to_local;
  var_to_local.reserve(g.n_vars * 2);
  for (size_t i = 0; i < g.n_vars; i++)
    var_to_local[r.vars[i]] = (int)i;

  for (size_t ci = 0; ci < g.n_cls; ci++) {
    int cl_local = (int)(g.n_vars + ci);
    for (unsigned v : r.long_clauses[ci].second) {
      auto it = var_to_local.find(v);
      if (it == var_to_local.end()) continue;
      g.adj[it->second].push_back(cl_local);
      g.adj[cl_local].push_back(it->second);
    }
  }
  for (const auto &p : r.binary_pairs) {
    auto ia = var_to_local.find(p.first);
    auto ib = var_to_local.find(p.second);
    if (ia == var_to_local.end() || ib == var_to_local.end()) continue;
    g.adj[ia->second].push_back(ib->second);
    g.adj[ib->second].push_back(ia->second);
  }
  return g;
}


static unsigned count_connected_components(const Graph &g) {
  if (g.n == 0) return 0;
  std::vector<bool> visited(g.n, false);
  unsigned cc = 0;
  std::queue<int> q;
  for (size_t i = 0; i < g.n; i++) {
    if (visited[i]) continue;
    cc++;
    visited[i] = true;
    q.push((int)i);
    while (!q.empty()) {
      int u = q.front(); q.pop();
      for (int v : g.adj[u]) {
        if (!visited[v]) { visited[v] = true; q.push(v); }
      }
    }
  }
  return cc;
}


enum class Outcome {
  TOO_SMALL,           // (1) n < 4
  METIS_ERROR,         // (2) METIS returned an error code
  DEGEN_EMPTY,         // (3a) METIS OK but separator is empty
  DEGEN_ONE_SIDED,     // (3b) one side has 0 vars
  ACCEPTED_TRIVIAL,    // (4) sep_size <= 2 (likely useless cut)
  ACCEPTED_GOOD,       // (5) sep_size > 2
};


static const char *outcome_name(Outcome o) {
  switch (o) {
    case Outcome::TOO_SMALL:        return "TOO_SMALL";
    case Outcome::METIS_ERROR:      return "METIS_ERROR";
    case Outcome::DEGEN_EMPTY:      return "DEGEN_EMPTY";
    case Outcome::DEGEN_ONE_SIDED:  return "DEGEN_ONE_SIDED";
    case Outcome::ACCEPTED_TRIVIAL: return "ACCEPTED_TRIVIAL";
    case Outcome::ACCEPTED_GOOD:    return "ACCEPTED_GOOD";
  }
  return "?";
}


struct ReplayResult {
  Outcome outcome = Outcome::METIS_ERROR;
  size_t  n_vars = 0;
  size_t  n_cls = 0;
  size_t  n = 0;
  size_t  edges = 0;          // undirected, deduplicated would be Σ|adj|/2
  unsigned cc = 0;            // connected components of the input graph
  size_t  sep_size = 0;
  size_t  left_vars = 0;
  size_t  right_vars = 0;
  double  metis_us = 0.0;
};


static ReplayResult replay(const Record &r) {
  ReplayResult res;
  res.n_vars = r.vars.size();
  res.n_cls  = r.long_clauses.size();
  res.n      = res.n_vars + res.n_cls;

  if (res.n < 4) {
    res.outcome = Outcome::TOO_SMALL;
    return res;
  }

  Graph g = build_graph(r);
  size_t total_deg = 0;
  for (const auto &a : g.adj) total_deg += a.size();
  res.edges = total_deg / 2;
  res.cc = count_connected_components(g);

  // Build CSR for METIS (mirror of computeRuntimeMetisSeparator).
  std::vector<idx_t> xadj(g.n + 1);
  std::vector<idx_t> adjncy;
  adjncy.reserve(total_deg);
  xadj[0] = 0;
  for (size_t i = 0; i < g.n; i++) {
    for (int u : g.adj[i]) adjncy.push_back(u);
    xadj[i + 1] = (idx_t)adjncy.size();
  }

  std::vector<idx_t> vwgt(g.n, 1);
  idx_t options[METIS_NOPTIONS];
  METIS_SetDefaultOptions(options);
  idx_t nvtxs = (idx_t)g.n;
  idx_t sepsize = 0;
  std::vector<idx_t> part(g.n);

  auto t0 = std::chrono::steady_clock::now();
  int ret = METIS_ComputeVertexSeparator(
      &nvtxs, xadj.data(), adjncy.data(),
      vwgt.data(), options, &sepsize, part.data());
  auto t1 = std::chrono::steady_clock::now();
  res.metis_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

  if (ret != METIS_OK) {
    res.outcome = Outcome::METIS_ERROR;
    return res;
  }

  size_t lv = 0, rv = 0, sep_vars = 0, sep_cls = 0;
  for (size_t i = 0; i < g.n; i++) {
    if (part[i] == 0) {
      if (i < g.n_vars) lv++;
    } else if (part[i] == 1) {
      if (i < g.n_vars) rv++;
    } else {
      if (i < g.n_vars) sep_vars++; else sep_cls++;
    }
  }
  res.left_vars = lv;
  res.right_vars = rv;
  res.sep_size = sep_vars + sep_cls;

  if (res.sep_size == 0) {
    res.outcome = Outcome::DEGEN_EMPTY;
  } else if (lv == 0 || rv == 0) {
    res.outcome = Outcome::DEGEN_ONE_SIDED;
  } else if (res.sep_size <= 2) {
    res.outcome = Outcome::ACCEPTED_TRIVIAL;
  } else {
    res.outcome = Outcome::ACCEPTED_GOOD;
  }
  return res;
}


static void print_summary(const std::vector<ReplayResult> &all) {
  if (all.empty()) {
    std::cout << "no records\n";
    return;
  }
  size_t total = all.size();

  // Outcome counts.
  std::unordered_map<int, size_t> count;
  for (const auto &r : all) count[(int)r.outcome]++;

  std::cout << "\n=== METIS replay summary (n=" << total << " records) ===\n";
  for (int o = 0; o < 6; o++) {
    Outcome ow = (Outcome)o;
    size_t c = count[(int)ow];
    if (c == 0) continue;
    double pct = 100.0 * c / total;
    std::cout << "  " << outcome_name(ow) << ": " << c
              << "  (" << std::fixed << std::setprecision(1) << pct << "%)\n";
  }

  // Per-outcome statistics.
  std::cout << "\n=== Per-outcome distributions ===\n";
  std::cout << "  outcome           | count |  n_avg |  cc=1 cc=2 cc=3 cc>3 | "
               "edges_avg | sep_avg | mean_us\n";
  for (int o = 0; o < 6; o++) {
    Outcome ow = (Outcome)o;
    std::vector<const ReplayResult *> rs;
    for (const auto &r : all) if (r.outcome == ow) rs.push_back(&r);
    if (rs.empty()) continue;
    double n_sum = 0, e_sum = 0, sep_sum = 0, us_sum = 0;
    size_t cc1 = 0, cc2 = 0, cc3 = 0, ccmore = 0;
    for (auto *r : rs) {
      n_sum += r->n;
      e_sum += r->edges;
      sep_sum += r->sep_size;
      us_sum += r->metis_us;
      if (r->cc == 1) cc1++;
      else if (r->cc == 2) cc2++;
      else if (r->cc == 3) cc3++;
      else if (r->cc > 3) ccmore++;
    }
    double na = n_sum / rs.size();
    double ea = e_sum / rs.size();
    double sa = sep_sum / rs.size();
    double ua = us_sum / rs.size();
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "  %-17s | %5zu | %6.1f | %5zu %4zu %4zu %4zu | %9.1f | %7.2f | %6.2f",
        outcome_name(ow), rs.size(), na, cc1, cc2, cc3, ccmore, ea, sa, ua);
    std::cout << buf << "\n";
  }

  // Connected-components histogram across ALL records.
  std::cout << "\n=== Connected-components count (all records) ===\n";
  std::unordered_map<unsigned, size_t> cc_hist;
  for (const auto &r : all) cc_hist[r.cc]++;
  std::vector<std::pair<unsigned, size_t>> v(cc_hist.begin(), cc_hist.end());
  std::sort(v.begin(), v.end());
  for (const auto &p : v) {
    if (p.first > 20) continue;  // cap for readability
    double pct = 100.0 * p.second / total;
    std::cout << "  cc=" << p.first << ": " << p.second
              << "  (" << std::fixed << std::setprecision(1) << pct << "%)\n";
  }

  // Ratio of degenerate-due-to-disconnected.
  size_t disconn_records = 0, disconn_failed = 0;
  for (const auto &r : all) {
    if (r.cc > 1) {
      disconn_records++;
      if (r.outcome == Outcome::DEGEN_EMPTY
          || r.outcome == Outcome::DEGEN_ONE_SIDED) {
        disconn_failed++;
      }
    }
  }
  if (disconn_records > 0) {
    double pct = 100.0 * disconn_failed / disconn_records;
    std::cout << "\n=== Disconnected inputs (cc > 1): " << disconn_records
              << "  (" << (100.0 * disconn_records / total) << "% of all)\n";
    std::cout << "  of these, degenerate METIS output: " << disconn_failed
              << "  (" << pct << "%)\n";
  }
}


int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::fprintf(stderr,
        "Usage: %s <dump-path> [--max N]\n"
        "  Reads dump records and replays each through METIS, then\n"
        "  prints summary statistics.\n",
        argv[0]);
    return 1;
  }
  std::string path = argv[1];
  size_t max_records = 0;  // 0 = unlimited
  for (int i = 2; i < argc; i++) {
    if (std::strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
      max_records = std::atoll(argv[++i]);
    }
  }

  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "cannot open: %s\n", path.c_str());
    return 1;
  }

  std::vector<ReplayResult> results;
  Record r;
  size_t read = 0;
  while (read_record(in, r)) {
    results.push_back(replay(r));
    read++;
    if (max_records && read >= max_records) break;
    if (read % 5000 == 0) {
      std::fprintf(stderr, "\r... %zu records replayed", read);
      std::fflush(stderr);
    }
  }
  std::fprintf(stderr, "\rread %zu records\n", read);

  print_summary(results);
  return 0;
}
