// nd_cost — build COCOA's full nested-dissection hierarchy on a CNF and
// estimate the decomposition's branching cost in log2(branch-leaves).
//
// This is the structural router signal that replaces the single-root-cut
// `sep_ratio`. We read the WHOLE ND tree COCOA would build (NDHierarchy::build,
// the exact same recursive METIS bisection the solver uses) and evaluate the
// recurrence
//
//     cost(C) = 2^{|Sep(C)|} * sum_children cost(child),   leaf cost = 2^{n_vars(leaf)}
//
// in log2 space (it overflows otherwise):
//
//     L(leaf)     = n_vars(leaf)
//     L(internal) = |Sep|_vars + log2( 2^{L(left)} + 2^{L(right)} )
//
// L(root) is the cost, in BITS (log2 of the estimated number of branch-leaves
// a decomposition-guided search visits, ignoring caching and BCP). Low L(root)
// => the decomposition collapses the formula into trivial independent pieces
// (route to COCOA); high L(root) => the tree never really decomposes the hard
// core (route to Ganak), even when the ROOT cut looked good.
//
// Branchings are counted on VARIABLES only (var separator elements; leaf var
// counts), to match the variable-based cost model. In bipartite mode a
// separator may also contain long-clause nodes — these are reported separately
// (nd_total_sep_clauses) but not charged as branchings.
//
// Output: one `key=value` per line on stdout (same convention as
// metis_features). On failure, nd_status != ok and a non-zero exit code.
//
// Usage: nd_cost <cnf-file> [--vars-only]

#include "nd_hierarchy.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

static void emit(const char* key, long long val) { std::printf("%s=%lld\n", key, val); }
static void emit(const char* key, double val) { std::printf("%s=%.6f\n", key, val); }
static void emit(const char* key, const char* val) { std::printf("%s=%s\n", key, val); }

// log2(2^a + 2^b), overflow-safe.
static double logsumexp2(double a, double b) {
  double m = a > b ? a : b;
  double o = a > b ? b : a;
  return m + std::log2(1.0 + std::pow(2.0, o - m));
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <cnf-file> [--vars-only]\n", argv[0]);
    return 2;
  }
  bool vars_only = false;
  for (int i = 2; i < argc; i++)
    if (std::string(argv[i]) == "--vars-only") vars_only = true;

  std::ifstream in(argv[1]);
  if (!in) {
    std::fprintf(stderr, "ERROR: cannot open %s\n", argv[1]);
    emit("nd_status", "open_failed");
    return 2;
  }

  // --- parse DIMACS ---
  int n_vars = 0, n_cls_decl = 0;
  std::vector<std::vector<unsigned>> clause_vars;  // distinct vars per clause
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == 'c') continue;
    if (line[0] == 'p') { std::sscanf(line.c_str(), "p cnf %d %d", &n_vars, &n_cls_decl); continue; }
    std::istringstream iss(line);
    std::set<unsigned> vs;
    int lit;
    while (iss >> lit && lit != 0) {
      unsigned v = (unsigned)std::abs(lit);
      if (v >= 1 && (int)v <= n_vars) vs.insert(v);
    }
    if (!vs.empty()) clause_vars.emplace_back(vs.begin(), vs.end());
  }

  // --- assemble build() inputs (mirrors Solver::buildIncidenceInputs):
  //     size>=3 -> long_clauses ; size==2 -> binary_pairs (deduped) ; size<=1 skipped.
  std::vector<std::pair<unsigned, std::vector<unsigned>>> long_clauses;
  std::vector<std::pair<unsigned, unsigned>> binary_pairs;
  std::set<std::pair<unsigned, unsigned>> bin_seen;
  std::unordered_set<unsigned> vars_in_clause;
  unsigned ofs = 0;
  for (auto& vs : clause_vars) {
    for (unsigned v : vs) vars_in_clause.insert(v);
    if (vs.size() >= 3) {
      long_clauses.push_back({ofs++, vs});
    } else if (vs.size() == 2) {
      unsigned a = vs[0], b = vs[1];
      if (a > b) std::swap(a, b);
      if (bin_seen.insert({a, b}).second) binary_pairs.push_back({a, b});
    }
  }
  const long long n_constrained = (long long)vars_in_clause.size();

  emit("nd_n_vars", (long long)n_vars);
  emit("nd_n_clauses", (long long)clause_vars.size());
  emit("nd_n_constrained_vars", n_constrained);
  emit("nd_vars_only", vars_only ? 1LL : 0LL);

  if (n_vars < 4 || (long long)clause_vars.size() < 2) {
    emit("nd_status", "too_small");
    emit("nd_log2_cost", -1.0);
    return 0;
  }

  // --- build the hierarchy (same construction the solver uses) ---
  NDHierarchy nd;
  const auto t0 = std::chrono::steady_clock::now();
  nd.build(n_vars, long_clauses, binary_pairs, vars_only);
  const double wall_s = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - t0).count();
  emit("nd_wall_time_s", wall_s);

  if (!nd.valid) {
    emit("nd_status", "build_failed");
    emit("nd_log2_cost", -1.0);
    return 3;
  }

  const int N = nd.n_nodes;

  // Per-node separator var/clause counts; the set of all separator vars.
  std::vector<int> sep_var_sz(N, 0), sep_cls_sz(N, 0);
  std::unordered_set<unsigned> sep_vars;
  long long total_sep_vars = 0, total_sep_clauses = 0;
  for (int i = 0; i < N; i++) {
    for (const auto& cn : nd.separator[i]) {
      if (cn.kind == CutNode::VAR) { sep_var_sz[i]++; sep_vars.insert(cn.id); total_sep_vars++; }
      else { sep_cls_sz[i]++; total_sep_clauses++; }
    }
  }

  // Per-leaf TRUE variable count: constrained vars assigned to that leaf that
  // are NOT separator vars (var_leaf tags separator vars to a leaf too).
  std::vector<long long> leaf_vars(nd.npes, 0);
  for (unsigned v = 1; (int)v <= n_vars; v++) {
    if (!vars_in_clause.count(v)) continue;     // free var: trivial, no branching
    if (sep_vars.count(v)) continue;            // counted as a separator branching
    int leaf = (v < nd.var_leaf.size()) ? nd.var_leaf[v] : -1;
    if (leaf >= 0 && leaf < nd.npes) leaf_vars[leaf]++;
  }

  // --- post-order: L(root) in log2, plus the heaviest root->leaf path ---
  std::vector<double> Lcost(N, 0.0), Lpath(N, 0.0);
  long long n_leaves = 0, n_internal = 0, n_passthrough = 0, worst_leaf = 0;
  long long sum_leaf_vars = 0;
  std::function<void(int)> walk = [&](int node) {
    if (node < 0) return;
    int lc = nd.left_child[node], rc = nd.right_child[node];
    if (lc < 0 && rc < 0) {                      // leaf
      long long lv = (nd.leaf_lo[node] >= 0 && nd.leaf_lo[node] < nd.npes)
                         ? leaf_vars[nd.leaf_lo[node]] : 0;
      Lcost[node] = (double)lv;
      Lpath[node] = (double)lv;
      if (lv > worst_leaf) worst_leaf = lv;
      sum_leaf_vars += lv;
      n_leaves++;
      return;
    }
    if (lc >= 0) walk(lc);
    if (rc >= 0) walk(rc);
    int s = sep_var_sz[node];                    // branchings = separator VARS
    if (s == 0) n_passthrough++; else n_internal++;
    double childLse, childPathMax;
    if (lc >= 0 && rc >= 0) {
      childLse = logsumexp2(Lcost[lc], Lcost[rc]);
      childPathMax = Lpath[lc] > Lpath[rc] ? Lpath[lc] : Lpath[rc];
    } else if (lc >= 0) {
      childLse = Lcost[lc]; childPathMax = Lpath[lc];
    } else {
      childLse = Lcost[rc]; childPathMax = Lpath[rc];
    }
    Lcost[node] = (double)s + childLse;
    Lpath[node] = (double)s + childPathMax;
  };
  walk(nd.root());

  // Accounting: every constrained var is either a separator var or a leaf var.
  const long long accounted = total_sep_vars + sum_leaf_vars;
  const bool acct_ok = (accounted == n_constrained);

  emit("nd_status", "ok");
  emit("nd_log2_cost", Lcost[nd.root()]);
  emit("nd_max_path_sep", Lpath[nd.root()]);     // bottleneck (sum sep + leaf on heaviest path)
  emit("nd_root_sep_vars", (long long)sep_var_sz[nd.root()]);
  emit("nd_worst_leaf_vars", worst_leaf);
  emit("nd_n_nodes", (long long)N);
  emit("nd_n_internal", n_internal);
  emit("nd_n_passthrough", n_passthrough);
  emit("nd_n_leaves", n_leaves);
  emit("nd_total_sep_vars", total_sep_vars);
  emit("nd_total_sep_clauses", total_sep_clauses);
  emit("nd_acct_ok", acct_ok ? 1LL : 0LL);
  if (!acct_ok) {
    emit("nd_acct_sum", accounted);
    std::fprintf(stderr,
        "WARNING: var accounting mismatch: sep_vars(%lld)+leaf_vars(%lld)=%lld != constrained(%lld)\n",
        total_sep_vars, sum_leaf_vars, accounted, n_constrained);
  }
  return 0;
}
