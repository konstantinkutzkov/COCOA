// test_probe — unit harness for Phase 1 primitives.
//
// For each (CNF, .expected) pair on the command line, loads the CNF via
// Solver::initForTesting (unit-clause propagation, no preprocessing),
// parses assertions from the expected file, runs Solver::probeLiteral
// against each asserted (var, polarity) pair, and compares results.
//
// Usage: test_probe <cnf> <expected> [<cnf> <expected> ...]

#include "solver.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using std::string;
using std::vector;
using std::set;

struct ProbeExpect {
    bool has_success = false, success;
    bool has_vars_forced = false;    int vars_forced;
    bool has_delta_2clauses = false; int delta_2clauses;
    bool has_uip_clause = false;     set<int> uip_clause;
};

struct ExpectedFile {
    bool has_model_count = false;
    long long model_count;
    // key: (var, sign) where sign=true means positive polarity (T)
    std::map<std::pair<int, bool>, ProbeExpect> probes;
};

static bool parse_bool(const string &s, bool &out) {
    if (s == "true")  { out = true;  return true; }
    if (s == "false") { out = false; return true; }
    return false;
}

static bool parse_uip_set(const string &s, set<int> &out) {
    // Expected form: {lit1 lit2 ...} or {lit1,lit2,...}.
    auto lb = s.find('{');
    auto rb = s.find('}');
    if (lb == string::npos || rb == string::npos || rb <= lb) return false;
    string body = s.substr(lb + 1, rb - lb - 1);
    for (auto &c : body) if (c == ',') c = ' ';
    std::istringstream iss(body);
    int v;
    while (iss >> v) out.insert(v);
    return true;
}

// parse key of the form "probe.V=P.FIELD" → (V, sign, field)
static bool parse_probe_key(const string &key, int &var, bool &sign,
                            string &field) {
    if (key.compare(0, 6, "probe.") != 0) return false;
    size_t eq = key.find('=', 6);
    if (eq == string::npos) return false;
    size_t dot = key.find('.', eq);
    if (dot == string::npos) return false;
    var = std::atoi(key.substr(6, eq - 6).c_str());
    char p = key[eq + 1];
    if (p != 'T' && p != 'F') return false;
    sign = (p == 'T');
    field = key.substr(dot + 1);
    return true;
}

static ExpectedFile parse_expected(const string &path) {
    ExpectedFile exp;
    std::ifstream in(path);
    if (!in) { std::cerr << "cannot open " << path << "\n"; std::exit(2); }
    string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != string::npos) line = line.substr(0, hash);
        std::istringstream iss(line);
        string key;
        if (!(iss >> key)) continue;
        // Collect the rest of the line as the value (for uip_clause sets
        // which may contain spaces inside braces).
        string value;
        std::getline(iss, value);
        // Trim leading whitespace from value.
        size_t i = 0; while (i < value.size() && std::isspace((unsigned char)value[i])) ++i;
        value = value.substr(i);

        if (key == "model_count") {
            exp.has_model_count = true;
            exp.model_count = std::atoll(value.c_str());
            continue;
        }
        int var; bool sign; string field;
        if (!parse_probe_key(key, var, sign, field)) continue;
        auto &pe = exp.probes[{var, sign}];
        if (field == "success") {
            if (!parse_bool(value, pe.success)) {
                std::cerr << "bad bool in " << path << ": " << line << "\n";
                std::exit(2);
            }
            pe.has_success = true;
        } else if (field == "vars_forced") {
            pe.vars_forced = std::atoi(value.c_str());
            pe.has_vars_forced = true;
        } else if (field == "delta_2clauses") {
            pe.delta_2clauses = std::atoi(value.c_str());
            pe.has_delta_2clauses = true;
        } else if (field == "uip_clause") {
            if (!parse_uip_set(value, pe.uip_clause)) {
                std::cerr << "bad uip set in " << path << ": " << line << "\n";
                std::exit(2);
            }
            pe.has_uip_clause = true;
        }
        // unknown fields are ignored
    }
    return exp;
}

static int run_case(const string &cnf, const string &expected_path) {
    ExpectedFile exp = parse_expected(expected_path);

    Solver solver;
    solver.config().quiet = true;
    solver.config().perform_pre_processing = false;

    bool init_ok = solver.initForTesting(cnf);
    // If init detects UNSAT via unit clauses, probes are not meaningful.
    if (!init_ok) {
        if (exp.has_model_count && exp.model_count == 0) {
            std::cout << "PASS " << cnf << " (UNSAT at init, matches expected)\n";
            return 0;
        }
        std::cerr << "FAIL " << cnf << ": initForTesting returned UNSAT but "
                  << "expected model_count != 0\n";
        return 1;
    }

    int fails = 0;
    for (const auto &kv : exp.probes) {
        int var  = kv.first.first;
        bool sig = kv.first.second;
        const ProbeExpect &pe = kv.second;
        LiteralID lit(var, sig);
        const std::string label = "probe.x" + std::to_string(var) + "="
                                + (sig ? "T" : "F");

        ProbeResult pr = solver.probeLiteral(lit);

        if (pe.has_success && pe.success != pr.success) {
            std::cerr << "FAIL " << cnf << " " << label << ".success: "
                      << "expected=" << (pe.success ? "true" : "false")
                      << " actual=" << (pr.success ? "true" : "false") << "\n";
            fails++;
            continue;
        }
        if (pr.success) {
            if (pe.has_vars_forced && pe.vars_forced != pr.vars_forced) {
                std::cerr << "FAIL " << cnf << " " << label << ".vars_forced: "
                          << "expected=" << pe.vars_forced
                          << " actual="   << pr.vars_forced << "\n";
                fails++;
            }
            if (pe.has_delta_2clauses && pe.delta_2clauses != pr.delta_2clauses) {
                std::cerr << "FAIL " << cnf << " " << label << ".delta_2clauses: "
                          << "expected=" << pe.delta_2clauses
                          << " actual="   << pr.delta_2clauses << "\n";
                fails++;
            }
        } else {
            if (pe.has_uip_clause) {
                // uip_clauses_ on failure holds the UIP sequence; the 1-UIP
                // is uip_clauses_.front(). Compare as a set of ints.
                set<int> actual;
                const auto &uip_view = solver.uip_clauses_view();
                if (!uip_view.empty()) {
                    for (LiteralID l : uip_view.front()) actual.insert(l.toInt());
                }
                if (actual != pe.uip_clause) {
                    std::cerr << "FAIL " << cnf << " " << label << ".uip_clause: "
                              << "expected={";
                    for (int l : pe.uip_clause) std::cerr << l << " ";
                    std::cerr << "} actual={";
                    for (int l : actual) std::cerr << l << " ";
                    std::cerr << "}\n";
                    fails++;
                }
            }
        }
    }

    if (fails == 0) {
        std::cout << "PASS " << cnf << "\n";
        return 0;
    }
    std::cerr << fails << " failure(s) in " << cnf << "\n";
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 3 || (argc % 2) == 0) {
        std::cerr << "Usage: test_probe <cnf> <expected> [<cnf> <expected> ...]\n";
        return 2;
    }
    int total_fails = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        total_fails += run_case(argv[i], argv[i + 1]);
    }
    if (total_fails == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << total_fails << " case(s) failed\n";
    return 1;
}
