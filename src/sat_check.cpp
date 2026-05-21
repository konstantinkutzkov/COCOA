/*
 * sat_check.cpp
 *
 * C++17 implementation of the SatChecker wrapper. The CMS headers
 * require C++17; this file is the *only* one that pulls them in. The
 * CMakeLists.txt overrides the project-level -std=c++11 for this file
 * with -std=c++17 (see src/CMakeLists.txt — same pattern as
 * preprocessor_light.cpp).
 */
#include "sat_check.h"

#include <cryptominisat.h>

struct SatChecker::Impl {
	CMSat::SATSolver solver;
};

SatChecker::SatChecker() : impl_(new Impl()) {
	impl_->solver.set_verbosity(0);
	impl_->solver.set_no_simplify_at_startup();
	impl_->solver.set_bve(0);
	impl_->solver.set_no_bva();
}

SatChecker::~SatChecker() = default;

static inline CMSat::Lit to_cms_lit(int dimacs_lit) {
	const unsigned v = (unsigned)(dimacs_lit > 0 ? dimacs_lit : -dimacs_lit) - 1;
	const bool     neg = (dimacs_lit < 0);
	return CMSat::Lit(v, neg);
}

void SatChecker::init(unsigned n_vars,
                       const std::vector<std::vector<int>>& clauses) {
	// CMS uses 0-indexed vars; our DIMACS uses 1..n_vars. Declare
	// n_vars CMS vars so to_cms_lit (which subtracts 1) lines up.
	impl_->solver.new_vars(n_vars);

	std::vector<CMSat::Lit> clause;
	for (const auto &c : clauses) {
		clause.clear();
		for (int dlit : c) clause.push_back(to_cms_lit(dlit));
		impl_->solver.add_clause(clause);
	}
}

SatChecker::Result SatChecker::check(const std::vector<int>& assumptions,
                                      unsigned max_confl) {
	std::vector<CMSat::Lit> assump;
	assump.reserve(assumptions.size());
	for (int dlit : assumptions) assump.push_back(to_cms_lit(dlit));

	impl_->solver.set_max_confl(max_confl);
	CMSat::lbool result = impl_->solver.solve(&assump);

	if (result == CMSat::l_False) return UNSAT;
	if (result == CMSat::l_True)  return SAT;
	return UNDEF;
}
