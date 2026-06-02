// metis_features — run METIS_ComputeVertexSeparator on a CNF's bipartite
// incidence graph and print machine-parseable structural features that
// the portfolio router uses for solver-selection.
//
// Output: one `key=value` per line, written to stdout. On any failure,
// `metis_status` is set to a non-`ok` value and the process exits with
// a non-zero status code.
//
// Built via CMakeLists.txt alongside metis_replay.
//
// Usage: metis_features <cnf-file>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <metis.h>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static void emit(const char* key, long long val) {
  std::printf("%s=%lld\n", key, val);
}
static void emit(const char* key, double val) {
  std::printf("%s=%.6f\n", key, val);
}
static void emit(const char* key, const char* val) {
  std::printf("%s=%s\n", key, val);
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <cnf-file>\n", argv[0]);
    return 2;
  }

  std::ifstream in(argv[1]);
  if (!in) {
    std::fprintf(stderr, "ERROR: cannot open %s\n", argv[1]);
    emit("metis_status", "open_failed");
    return 2;
  }

  int n_vars = 0, n_cls_decl = 0;
  std::vector<std::vector<int>> clauses;

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == 'c') continue;
    if (line[0] == 'p') {
      std::sscanf(line.c_str(), "p cnf %d %d", &n_vars, &n_cls_decl);
      continue;
    }
    std::istringstream iss(line);
    std::vector<int> cls;
    int lit;
    while (iss >> lit && lit != 0) cls.push_back(std::abs(lit));
    if (!cls.empty()) clauses.push_back(cls);
  }
  const int n_cls = static_cast<int>(clauses.size());

  emit("metis_n_vars", static_cast<long long>(n_vars));
  emit("metis_n_clauses", static_cast<long long>(n_cls));

  // METIS_ComputeVertexSeparator requires nvtxs >= 4. Trivially-small
  // formulas don't benefit from routing anyway — emit a clean record and
  // exit OK so the wrapper sees a successful run with sentinel values.
  if (n_vars < 4 || n_cls < 2) {
    emit("metis_status", "too_small");
    emit("metis_sepsize", -1LL);
    emit("metis_sep_vars", -1LL);
    emit("metis_left_vars", -1LL);
    emit("metis_right_vars", -1LL);
    emit("metis_balance", -1.0);
    emit("metis_sep_ratio", -1.0);
    emit("metis_wall_time_s", 0.0);
    return 0;
  }

  // Bipartite incidence graph:
  //   nodes 0..n_vars-1     are variables
  //   nodes n_vars..n-1     are clauses
  const int n_nodes = n_vars + n_cls;

  std::vector<std::set<int>> adj(n_nodes);
  for (int ci = 0; ci < n_cls; ci++) {
    const int clause_node = n_vars + ci;
    for (int v : clauses[ci]) {
      const int var_node = v - 1;  // 0-indexed
      if (var_node < 0 || var_node >= n_vars) continue;
      adj[var_node].insert(clause_node);
      adj[clause_node].insert(var_node);
    }
  }

  std::vector<idx_t> xadj(n_nodes + 1);
  std::vector<idx_t> adjncy;
  xadj[0] = 0;
  for (int i = 0; i < n_nodes; i++) {
    for (int j : adj[i]) adjncy.push_back(j);
    xadj[i + 1] = adjncy.size();
  }
  std::vector<idx_t> vwgt(n_nodes, 1);

  idx_t options[METIS_NOPTIONS];
  METIS_SetDefaultOptions(options);

  idx_t nvtxs = n_nodes;
  idx_t sepsize = 0;
  std::vector<idx_t> part(n_nodes);

  const auto t0 = std::chrono::steady_clock::now();
  const int ret = METIS_ComputeVertexSeparator(
      &nvtxs, xadj.data(), adjncy.data(),
      vwgt.data(), options, &sepsize, part.data());
  const double wall_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();

  if (ret != METIS_OK) {
    emit("metis_status", "metis_failed");
    emit("metis_ret", static_cast<long long>(ret));
    emit("metis_wall_time_s", wall_s);
    return 3;
  }

  long long n_left = 0, n_right = 0, n_sep = 0;
  long long sep_vars = 0, sep_clauses = 0;
  long long left_vars = 0, right_vars = 0;
  for (int i = 0; i < n_nodes; i++) {
    if (part[i] == 0) {
      n_left++;
      if (i < n_vars) left_vars++;
    } else if (part[i] == 1) {
      n_right++;
      if (i < n_vars) right_vars++;
    } else {
      n_sep++;
      if (i < n_vars) sep_vars++;
      else sep_clauses++;
    }
  }

  const long long lr_sum = left_vars + right_vars;
  const double balance =
      (lr_sum > 0)
          ? static_cast<double>(std::min(left_vars, right_vars)) /
                static_cast<double>(lr_sum)
          : 0.0;
  // sep_ratio matches the convention in sharpsat-separator's Phase-2 gate:
  // (separator vars) / (active vars in component). Here, active = n_vars.
  const double sep_ratio =
      (n_vars > 0) ? static_cast<double>(sep_vars) / static_cast<double>(n_vars)
                   : 0.0;

  emit("metis_status", "ok");
  emit("metis_sepsize", static_cast<long long>(sepsize));
  emit("metis_n_left", n_left);
  emit("metis_n_right", n_right);
  emit("metis_n_sep", n_sep);
  emit("metis_sep_vars", sep_vars);
  emit("metis_sep_clauses", sep_clauses);
  emit("metis_left_vars", left_vars);
  emit("metis_right_vars", right_vars);
  emit("metis_balance", balance);
  emit("metis_sep_ratio", sep_ratio);
  emit("metis_wall_time_s", wall_s);

  return 0;
}
