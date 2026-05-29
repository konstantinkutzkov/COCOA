// Arjun preprocessing integration.
// See preprocessor_arjun.h for contract; ganak/src/main.cpp:381-403 for
// the reference invocation we mirror.

#include "preprocessor_arjun.h"

// Arjun + CryptoMiniSat5 are C++17.
#include <arjun/arjun.h>
#include <cryptominisat5/cryptominisat.h>
#include <cryptominisat5/solvertypesmini.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Read a DIMACS CNF from path and feed it into Arjun's SimplifiedCNF.
// Returns false on parse failure. Sets nvars and ncls for the caller.
bool loadDimacsIntoArjun(const std::string& path,
                        ArjunNS::SimplifiedCNF& cnf,
                        unsigned& nvars,
                        unsigned& ncls) {
  std::ifstream in(path);
  if (!in) {
    std::cerr << "[arjun] cannot open " << path << "\n";
    return false;
  }

  nvars = 0;
  ncls = 0;
  bool header_seen = false;
  std::string line;
  std::vector<CMSat::Lit> clause;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (line[0] == 'c') continue;
    if (line[0] == 'p') {
      // p cnf NVARS NCLS
      std::istringstream ss(line);
      std::string tok, cnf_tok;
      ss >> tok >> cnf_tok >> nvars >> ncls;
      header_seen = true;
      cnf.new_vars(nvars);
      continue;
    }
    if (!header_seen) continue;
    // clause line: literals followed by 0
    std::istringstream ss(line);
    clause.clear();
    int lit_int = 0;
    while (ss >> lit_int) {
      if (lit_int == 0) {
        cnf.add_clause(clause);
        clause.clear();
        continue;
      }
      const unsigned var0 = (unsigned)std::abs(lit_int) - 1;  // 0-indexed
      const bool neg = (lit_int < 0);
      clause.emplace_back(var0, neg);
    }
    // If the clause did not end with 0 on this line, the parse is
    // forgiving (clauses can span lines too — but standard DIMACS
    // single-line is what we expect from our writers).
  }
  return true;
}

}  // namespace

bool PreprocessorArjun::simplify(const std::string& input_path,
                                  const std::string& output_path,
                                  const std::string& sidecar_path,
                                  unsigned verbosity,
                                  bool all_indep) {
  // Field generator for integer mode (#SAT, not weighted).
  auto fg = std::make_unique<ArjunNS::FGenMpz>();
  ArjunNS::SimplifiedCNF cnf(fg.get());

  unsigned nvars = 0, ncls = 0;
  if (!loadDimacsIntoArjun(input_path, cnf, nvars, ncls)) {
    return false;
  }

  // Tell Arjun every variable starts in the sampling set; it will
  // shrink to a minimal independent support unless all_indep=true.
  std::vector<uint32_t> all_vars;
  all_vars.reserve(nvars);
  for (uint32_t v = 0; v < nvars; v++) all_vars.push_back(v);
  cnf.set_sampl_vars(all_vars);

  ArjunNS::Arjun arjun;
  arjun.set_verb(verbosity);

  arjun.standalone_minimize_indep(cnf, all_indep);

  const auto& sampl_vars = cnf.get_sampl_vars();
  const auto& clauses = cnf.get_clauses();
  const auto& multiplier = cnf.get_multiplier_weight();

  // Write simplified CNF (DIMACS, 1-indexed).
  {
    std::ofstream out(output_path);
    if (!out) {
      std::cerr << "[arjun] cannot write " << output_path << "\n";
      return false;
    }
    out << "c arjun simplified\n";
    out << "p cnf " << cnf.nVars() << " " << clauses.size() << "\n";
    for (const auto& cl : clauses) {
      for (const auto& l : cl) {
        const int dimacs = (int)(l.var() + 1) * (l.sign() ? -1 : 1);
        out << dimacs << " ";
      }
      out << "0\n";
    }
  }

  // Write sidecar.
  {
    std::ofstream out(sidecar_path);
    if (!out) {
      std::cerr << "[arjun] cannot write " << sidecar_path << "\n";
      return false;
    }
    out << "c arjun sidecar (vars 1-indexed to match output CNF)\n";
    out << "indep " << sampl_vars.size() << "\n";
    for (uint32_t v0 : sampl_vars) {
      out << (v0 + 1) << "\n";   // 1-indexed
    }
    // Multiplier — dynamic_cast to FMpz to access .val (mpz_class).
    const auto* fmpz = dynamic_cast<const ArjunNS::FMpz*>(multiplier.get());
    if (fmpz) {
      out << "multiplier " << fmpz->val.get_str() << "\n";
    } else {
      // Non-FMpz multiplier — only expected for weighted modes. Use 1
      // as a defensive default; caller should validate.
      std::cerr << "[arjun] non-FMpz multiplier; defaulting to 1\n";
      out << "multiplier 1\n";
    }
  }

  return true;
}
