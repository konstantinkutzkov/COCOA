#include "preprocessor_light.h"

#include <cryptominisat.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool read_dimacs_into(const std::string& path, CMSat::SATSolver& solver,
                     unsigned& n_vars_out) {
  std::ifstream in(path);
  if (!in) {
    std::cerr << "[arjun-light] cannot open input CNF: " << path << "\n";
    return false;
  }
  std::string line;
  bool header_seen = false;
  n_vars_out = 0;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (line[0] == 'c') continue;
    if (line[0] == 'p') {
      std::istringstream ss(line);
      std::string p, cnf;
      unsigned nv = 0, nc = 0;
      ss >> p >> cnf >> nv >> nc;
      if (cnf != "cnf") {
        std::cerr << "[arjun-light] expected 'p cnf', got: " << line << "\n";
        return false;
      }
      n_vars_out = nv;
      solver.new_vars(nv);
      header_seen = true;
      continue;
    }
    if (!header_seen) {
      std::cerr << "[arjun-light] clause before header in: " << path << "\n";
      return false;
    }
    std::istringstream ss(line);
    std::vector<CMSat::Lit> clause;
    int lit;
    while (ss >> lit) {
      if (lit == 0) {
        solver.add_clause(clause);
        clause.clear();
        continue;
      }
      unsigned v = (unsigned)(lit > 0 ? lit : -lit) - 1;
      bool neg = (lit < 0);
      clause.push_back(CMSat::Lit(v, neg));
    }
    // Some DIMACS files break a clause across multiple lines. We
    // committed any clause that ended with 0; literals accumulated
    // without a terminating 0 stay in `clause` and continue on the
    // next line. The outer loop handles that.
    // (We do nothing extra here; the next iteration reads more lits.)
  }
  return true;
}

}  // namespace

bool PreprocessorLight::simplify(const std::string& input_path,
                                 const std::string& output_path,
                                 unsigned verbosity,
                                 const std::string& equiv_sidecar_path) {
  CMSat::SATSolver solver;
  solver.set_verbosity(verbosity);
  solver.set_prefix("c o ");

  // Forbid CMS passes that change the *raw* model count.
  solver.set_bve(0);                    // No bounded variable elimination
  solver.set_no_bva();                  // No bounded variable addition
  solver.set_no_simplify_at_startup();  // We control simplification ourselves
  // SCC kept ON at the solver level. Probing (full-probe / intree-probe)
  // implicitly triggers SCC substitution even without an explicit
  // must-scc-vrepl token, so detected equivalences are absorbed by the
  // simplified formula's structure rather than left as redundant
  // binaries. The redundant-binary lane is therefore empty under this
  // config — the equivalences have already been enforced upstream.
  // On t1_011: set_scc(1) → 1917 vars; set_scc(0) → 2045 vars + 128
  // equivalences as binaries. The 128-var difference matters more for
  // search cost than the lane's BCP benefit can recover.
  solver.set_scc(1);
  // Renumber after simplification so the output CNF is compact.
  solver.set_renumber(true);

  unsigned n_vars_in = 0;
  if (!read_dimacs_into(input_path, solver, n_vars_in)) return false;

  // Backbone: identify lits that are TRUE in every satisfying assignment.
  // Adding them as units shrinks the residual without dropping any models.
  bool backbone_finished = false;
  solver.backbone_simpl(20 * 1000ULL, backbone_finished);

  // Strategy string: ONLY count-preserving AND structure-preserving passes.
  //
  // EXCLUDED for count-correctness: occ-bve, occ-bve-empty,
  // occ-rem-with-orgates, occ-ternary-res (BVE family); autarky; BCE; SBVA.
  //
  // EXCLUDED for structure-preservation: must-scc-vrepl. Equivalence
  // replacement y ← ±x fuses y's clause neighborhood onto x, creating
  // high-degree "hub" vars that bridge formerly-separable regions. That
  // hurts separator-based decomposition even though count is preserved.
  // (Verified empirically on t1_011: 71% var reduction with SCC-vrepl
  //  produced a CNF our solver couldn't finish in 120s, vs 13.5s on
  //  the original.) All remaining passes either remove edges (subsumption,
  //  vivification) or remove vertices (backbone, probing) — never fuse.
  std::string strategy =
      "full-probe, "
      "sub-impl, "
      "sub-cls-with-bin, "
      "distill-cls-onlyrem, "
      "occ-backw-sub, "
      "occ-backw-sub-str, "
      "sub-str-cls-with-bin, "
      "intree-probe, "
      "clean-cls, "
      "distill-cls, "
      "distill-bins, "
      "must-renumber";

  CMSat::lbool result = solver.simplify(nullptr, &strategy);

  std::ofstream out(output_path);
  if (!out) {
    std::cerr << "[arjun-light] cannot write output CNF: " << output_path
              << "\n";
    return false;
  }

  auto write_empty_sidecar = [&]() {
    if (equiv_sidecar_path.empty()) return;
    std::ofstream eqv_out(equiv_sidecar_path);
    if (eqv_out) {
      eqv_out << "c arjun-light equivalences (1-indexed vars, '+' = same polarity)\n";
      eqv_out << 0 << "\n";
    }
  };

  if (result == CMSat::l_False) {
    // UNSAT — emit the empty clause. Our solver will report count = 0.
    out << "p cnf 0 1\n0\n";
    write_empty_sidecar();
    return true;
  }

  // After must-renumber, backbone-fixed vars and SCC-replaced vars are
  // GONE from the simplified view — their effect is baked into the
  // smaller renumbered var space. So we do NOT re-emit zero-assigned
  // lits; doing so would either duplicate baked-in constraints or
  // reference invalid renumbered indices.
  std::vector<std::vector<CMSat::Lit>> simplified_clauses;
  solver.start_getting_constraints(/*red=*/false, /*simplified=*/true);
  std::vector<CMSat::Lit> cl;
  bool is_xor = false, rhs = false;
  while (solver.get_next_constraint(cl, is_xor, rhs)) {
    if (is_xor) continue;  // we don't add xors
    simplified_clauses.push_back(cl);
  }
  solver.end_getting_constraints();

  unsigned n_vars_out = solver.simplified_nvars();
  if (n_vars_out == 0) {
    // Every variable was eliminated (backbone + SCC). The original
    // formula has exactly one satisfying assignment. Emit a dummy
    // CNF with count = 1: one var forced by a unit clause.
    out << "p cnf 1 1\n1 0\n";
    (void)n_vars_in;
    write_empty_sidecar();
    return true;
  }

  out << "p cnf " << n_vars_out << " " << simplified_clauses.size() << "\n";
  for (const auto& clause : simplified_clauses) {
    for (const auto& l : clause) {
      int v = (int)l.var() + 1;
      out << (l.sign() ? -v : v) << " ";
    }
    out << "0\n";
  }
  out.close();

  // SCC equivalence extraction (second pass).
  //
  // We feed a FRESH CMS5 instance with the just-written simplified CNF
  // and run a single pass with SCC-vrepl enabled. CMS5's
  // get_all_binary_xors() then reports every equivalence x ↔ ±y it
  // proved from the simplified clause set. These equivalences are in
  // the SAME variable space as output_path. We write them to the
  // sidecar; main.cpp later hands them to the solver via
  // setPendingRedundantEquivalences for BCP-only injection.
  if (!equiv_sidecar_path.empty()) {
    CMSat::SATSolver scc_solver;
    scc_solver.set_verbosity(verbosity);
    scc_solver.set_prefix("c o ");
    scc_solver.set_bve(0);
    scc_solver.set_no_bva();
    scc_solver.set_no_simplify_at_startup();
    scc_solver.set_scc(1);
    scc_solver.set_renumber(false);   // keep var indices stable

    unsigned scc_n_vars = 0;
    if (!read_dimacs_into(output_path, scc_solver, scc_n_vars)) {
      std::cerr << "[arjun-light] SCC pass: failed to read simplified CNF\n";
      return false;
    }
    std::string scc_strategy = "must-scc-vrepl";
    scc_solver.simplify(nullptr, &scc_strategy);

    auto xors = scc_solver.get_all_binary_xors();

    std::ofstream eqv_out(equiv_sidecar_path);
    if (!eqv_out) {
      std::cerr << "[arjun-light] cannot write equivalence sidecar: "
                << equiv_sidecar_path << "\n";
      return false;
    }
    eqv_out << "c arjun-light equivalences (1-indexed vars, '+' = same polarity)\n";
    eqv_out << xors.size() << "\n";
    for (const auto& p : xors) {
      // p = (replaced, replaced_with). CMS5 returns equality of LITERALS;
      // two literals are equal iff their truth values match in every
      // model, so same-polarity iff p.first.sign() == p.second.sign().
      unsigned v1 = p.first.var() + 1;
      unsigned v2 = p.second.var() + 1;
      bool same_pol = (p.first.sign() == p.second.sign());
      eqv_out << v1 << " " << v2 << " " << (same_pol ? '+' : '-') << "\n";
    }
  }

  return true;
}
