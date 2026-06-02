// Solver-side wrapper around the standalone SCC + 2-SAT UNSAT detector
// (see scc_unsat.h). Extracts the residual binary-implication graph
// from the Solver's current state — active vars, binary_links_, and
// long clauses in `comp` shortened by the trail — then defers to
// SccUnsat::is_unsat_2sat for the actual check.
//
// Kept in its own translation unit so the SCC algorithm itself stays
// fully decoupled from Solver/Instance/Component types and can be
// unit-tested in isolation (tests/test_scc_unsat.cpp).

#include "scc_unsat.h"
#include "solver.h"
#include "component_types/component.h"
#include "structures.h"

#include <utility>
#include <vector>

namespace {

// Convert a (var, sign) pair using a dense remapping into a DIMACS-
// style signed int. Dense IDs are 1-indexed (1..k); sign=true -> +id,
// sign=false -> -id.
inline int dimacs_from(int dense_id_1based, bool sign) {
    return sign ? dense_id_1based : -dense_id_1based;
}

}  // namespace

bool Solver::sccCheckComponentUnsat(Component &comp) {
    if (comp.num_variables() == 0) return false;

    // Dense remapping: var-id (in the global 1..num_variables() space)
    // -> dense 1-indexed id in [1, k]. -1 = not in the residual.
    //
    // Done so the SCC graph has 2k literal nodes (one per active var
    // polarity), not 2 * num_variables() — a meaningful saving once
    // the search has been running for a while and most vars are
    // assigned.
    const unsigned n_global = num_variables();
    std::vector<int> dense(n_global + 1, -1);
    int k = 0;
    for (auto v_it = comp.varsBegin(); *v_it != varsSENTINEL; ++v_it) {
        const unsigned v = *v_it;
        if (isActive(LiteralID(v, true))) {
            ++k;
            dense[v] = k;  // 1-indexed
        }
    }
    if (k == 0) return false;

    std::vector<std::pair<int, int> > binaries;
    std::vector<int> units;

    // Active ORIGINAL binary clauses (a v b) with both literals X_TRI.
    //
    // Each binary is stored at both endpoints in binary_links_. Walk
    // from each endpoint but emit only when l.raw() < partner.raw()
    // to de-dup. Skip clauses where either literal is satisfied.
    //
    // **Only walk the original lane** (idx < original_binary_link_count_).
    // Learned binaries can carry scope conditions (added by
    // addScopedUIPConflictClause) — treating them as unconditional in
    // the SCC graph fabricates implications that aren't really active
    // in the current scope, leading to spurious UNSAT verdicts.
    // canonical_key.cpp uses the same filter (canonical_key.cpp:179);
    // brute-force does too. Original binaries are unconditionally
    // sound consequences of the original CNF and safe to include.
    for (auto v_it = comp.varsBegin(); *v_it != varsSENTINEL; ++v_it) {
        const unsigned v = *v_it;
        if (dense[v] < 0) continue;
        for (int sign_i = 0; sign_i < 2; ++sign_i) {
            const LiteralID l(v, sign_i == 1);
            const Literal &lit_rec = literal(l);
            const unsigned orig_count = lit_rec.original_binary_link_count_;
            unsigned idx = 0;
            for (auto bt = lit_rec.binary_links_.begin();
                 *bt != SENTINEL_LIT; ++bt, ++idx) {
                if (idx >= orig_count) break;  // skip learned binaries
                const LiteralID p = *bt;
                if (p.raw() <= l.raw()) continue;  // de-dup
                // If either side is resolved, the binary degenerates
                // to a unit (the surviving literal). BCP should have
                // already propagated it, but be defensive.
                const bool l_sat = isSatisfied(l);
                const bool p_sat = isSatisfied(p);
                if (l_sat || p_sat) continue;  // clause satisfied
                const bool l_res = isResolved(l);
                const bool p_res = isResolved(p);
                if (l_res && p_res) {
                    // Both falsified — degenerate UNSAT clause, but
                    // BCP would have raised a conflict before us.
                    // Skip silently.
                    continue;
                }
                if (l_res) {
                    const int p_dense = dense[p.var()];
                    if (p_dense > 0) units.push_back(
                        dimacs_from(p_dense, p.sign()));
                    continue;
                }
                if (p_res) {
                    const int l_dense = dense[l.var()];
                    if (l_dense > 0) units.push_back(
                        dimacs_from(l_dense, l.sign()));
                    continue;
                }
                // Both active. Add the binary.
                const int l_dense = dense[l.var()];
                const int p_dense = dense[p.var()];
                if (l_dense <= 0 || p_dense <= 0) continue;
                binaries.push_back(std::make_pair(
                    dimacs_from(l_dense, l.sign()),
                    dimacs_from(p_dense, p.sign())));
            }
        }
    }

    // Long clauses in `comp`. Walk literals; skip clause if any is
    // satisfied; collect active literals. If count == 2 -> add as a
    // binary. If count == 1 -> unit (defensive; BCP would normally
    // have propagated). If count == 0 -> empty -> UNSAT directly.
    // If count >= 3 -> not a 2-SAT contribution.
    //
    // **Skip removed clauses** (in `removed_clauses_`). A clause that
    // has been "consumed" via branchOnClause's consume arm is no
    // longer a constraint — including its (possibly shortened) edges
    // in the SCC graph re-asserts a dropped constraint and can
    // fabricate UNSAT certificates that don't apply to the residual.
    // canonical_key.cpp filters the same way (canonical_key.cpp:145);
    // brute-force does too (solver.cpp:1441 via isClauseRemoved).
    for (auto cl_it = comp.clsBegin(); *cl_it != clsSENTINEL; ++cl_it) {
        const ClauseOfs ofs = comp_manager_.clauseOfsOf(*cl_it);
        if (isClauseRemoved(ofs)) continue;
        bool satisfied = false;
        int active_lits[3];  // we only need up to 3 to discriminate
        int n_active = 0;
        for (auto lt = beginOf(ofs); *lt != SENTINEL_LIT; ++lt) {
            if (isSatisfied(*lt)) { satisfied = true; break; }
            if (isResolved(*lt)) continue;
            if (n_active < 3) active_lits[n_active] = static_cast<int>(lt->raw());
            ++n_active;
        }
        if (satisfied) continue;
        if (n_active == 0) {
            // Empty residual clause -> immediate UNSAT. Shouldn't
            // happen post-BCP but if it does, that's the answer.
            return true;
        }
        if (n_active == 1) {
            LiteralID a;
            a.copyRaw(static_cast<unsigned>(active_lits[0]));
            const int a_dense = dense[a.var()];
            if (a_dense > 0) units.push_back(dimacs_from(a_dense, a.sign()));
            continue;
        }
        if (n_active == 2) {
            LiteralID a, b;
            a.copyRaw(static_cast<unsigned>(active_lits[0]));
            b.copyRaw(static_cast<unsigned>(active_lits[1]));
            const int a_dense = dense[a.var()];
            const int b_dense = dense[b.var()];
            if (a_dense <= 0 || b_dense <= 0) continue;
            binaries.push_back(std::make_pair(
                dimacs_from(a_dense, a.sign()),
                dimacs_from(b_dense, b.sign())));
            continue;
        }
        // n_active >= 3: no 2-SAT contribution.
    }

    return SccUnsat::is_unsat_2sat(static_cast<unsigned>(k), binaries, units);
}
