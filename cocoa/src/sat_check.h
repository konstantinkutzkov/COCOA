/*
 * sat_check.h
 *
 * Thin opaque wrapper around CryptoMiniSat for the Phase 1 SAT-check
 * diagnostic in Solver. The full <cryptominisat.h> include is confined
 * to sat_check.cpp (compiled as C++17); this header is C++11-clean so
 * the rest of solver.cpp can use it.
 */
#ifndef SAT_CHECK_H_
#define SAT_CHECK_H_

#include <memory>
#include <vector>

class SatChecker {
public:
	enum Result { UNSAT, SAT, UNDEF };

	SatChecker();
	~SatChecker();

	// Initialize the underlying CMS instance with `n_vars` variables
	// (1-indexed, var 0 unused) and the given clauses in DIMACS lit
	// convention (positive int = positive lit, negative = negated lit).
	// Called once after preprocessing.
	void init(unsigned n_vars,
	          const std::vector<std::vector<int>>& clauses);

	// Call CMS with the given assumptions (DIMACS-style ints) and a
	// per-call conflict budget. Returns UNSAT / SAT / UNDEF (conflict
	// budget exhausted before either was proven).
	Result check(const std::vector<int>& assumptions, unsigned max_confl);

private:
	// Pimpl so consumers never see CMS types.
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

#endif /* SAT_CHECK_H_ */
