/*
 * preprocessor.cpp
 *
 * See preprocessor.h for the semantic contract. This file owns its
 * own clause representation, occurrence lists, and BCP — no references
 * to Solver, Instance, or any other solver-facing state. A bug in
 * these rules can at most produce an incorrect output CNF; it cannot
 * corrupt the solver's in-memory state.
 */

#include "preprocessor.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <queue>

namespace {

// -------------------------------------------------------------------
// Utilities.
// -------------------------------------------------------------------

// 64-bit bloom bit for a DIMACS literal int. Used for fast reject in
// subsumption/SSR when the candidate's sig has a bit the superset lacks.
static inline uint64_t bloom_bit(int l) {
  unsigned r = (unsigned)(l < 0 ? -l : l);
  r ^= r >> 16; r *= 0x7feb352dU;
  r ^= r >> 15; r *= 0x846ca68bU;
  return 1ULL << (r & 63);
}

static inline uint64_t sig_of(const std::vector<int> &lits) {
  uint64_t s = 0;
  for (int l : lits) s |= bloom_bit(l);
  return s;
}

// Sorted-vector subset test: returns true iff every element of A is
// present in B. Both inputs must be sorted ascending.
static bool is_subset(const std::vector<int> &A, const std::vector<int> &B) {
  if (A.size() > B.size()) return false;
  size_t ai = 0, bi = 0;
  while (ai < A.size()) {
    while (bi < B.size() && B[bi] < A[ai]) ++bi;
    if (bi == B.size() || B[bi] != A[ai]) return false;
    ++ai; ++bi;
  }
  return true;
}

// Encode a DIMACS int into a compact key for occurrence-list indexing.
// Positive lit +v → key 2v; negative lit -v → key 2v+1. Valid for
// v >= 1; v == 0 is never a valid literal and need not be representable.
static inline unsigned lit_key(int l) {
  unsigned v = (unsigned)(l < 0 ? -l : l);
  return (v << 1) | (l < 0 ? 1u : 0u);
}

// -------------------------------------------------------------------
// Clause pool.
// -------------------------------------------------------------------

struct InternalClause {
  std::vector<int> lits;   // sorted ascending; empty and deleted=true
                           // are the terminal states.
  uint64_t sig = 0;
  bool deleted = false;
};

struct State {
  unsigned n_vars = 0;
  std::vector<InternalClause> clauses;
  // Occurrence lists, indexed by lit_key(l).
  std::vector<std::vector<unsigned>> occ;
  // Variable values: 0=unset, 1=true, -1=false.
  std::vector<int8_t> val;
  // Order-preserving list of forced literals (DIMACS ints).
  std::vector<int> forced;
  // BCP queue of literals forced true but not yet propagated.
  std::queue<int> bcp_queue;
  bool unsat = false;

  void rebuild_occ() {
    occ.assign(2u * (n_vars + 2u), {});
    for (unsigned ci = 0; ci < clauses.size(); ci++) {
      if (clauses[ci].deleted) continue;
      for (int l : clauses[ci].lits)
        occ[lit_key(l)].push_back(ci);
    }
  }

  // Force literal `l` to true. Returns false iff this produces an
  // immediate conflict with an existing assignment.
  bool force(int l) {
    int v = (l < 0) ? -l : l;
    int desired = (l < 0) ? -1 : 1;
    if (val[v] != 0) {
      if (val[v] != desired) { unsat = true; return false; }
      return true;   // redundant — already set to same value
    }
    val[v] = desired;
    forced.push_back(l);
    bcp_queue.push(l);
    return true;
  }

  // Propagate the current BCP queue to saturation. Removes satisfied
  // clauses (mark deleted) and removes falsified literals from
  // survivors. A clause that becomes a unit queues another force.
  // After return, `occ` is rebuilt so rule passes can rely on it.
  //
  // Returns false iff a conflict (empty clause or contradictory force)
  // was detected.
  bool bcp() {
    while (!bcp_queue.empty()) {
      int l = bcp_queue.front();
      bcp_queue.pop();
      // Clauses containing l (sat).
      for (unsigned ci : occ[lit_key(l)]) {
        if (clauses[ci].deleted) continue;
        clauses[ci].deleted = true;
      }
      // Clauses containing -l (remove -l; may become unit/empty).
      for (unsigned ci : occ[lit_key(-l)]) {
        if (clauses[ci].deleted) continue;
        auto &lits = clauses[ci].lits;
        auto it = std::find(lits.begin(), lits.end(), -l);
        if (it == lits.end()) continue;   // stale occ entry, skip
        lits.erase(it);
        clauses[ci].sig = sig_of(lits);
        if (lits.empty()) { unsat = true; return false; }
        if (lits.size() == 1) {
          int u = lits[0];
          clauses[ci].deleted = true;
          if (!force(u)) return false;
        }
      }
    }
    rebuild_occ();
    return true;
  }
};

// -------------------------------------------------------------------
// Rule passes.
// -------------------------------------------------------------------

// Subsumption. Returns true iff at least one clause was deleted.
// Precondition: occ is up to date.
static bool pass_subsumption(State &st, unsigned long &stat) {
  unsigned ndel = 0;
  for (unsigned ci = 0; ci < st.clauses.size(); ci++) {
    if (st.clauses[ci].deleted) continue;
    const auto &C = st.clauses[ci];
    // Pick C's rarest-occurring literal to minimize the candidate set.
    unsigned best_key = lit_key(C.lits[0]);
    size_t best_sz = st.occ[best_key].size();
    for (int l : C.lits) {
      unsigned k = lit_key(l);
      if (st.occ[k].size() < best_sz) {
        best_sz = st.occ[k].size();
        best_key = k;
      }
    }
    for (unsigned di : st.occ[best_key]) {
      if (di == ci) continue;
      if (st.clauses[di].deleted) continue;
      const auto &D = st.clauses[di];
      if (D.lits.size() < C.lits.size()) continue;
      if ((C.sig & ~D.sig) != 0) continue;  // bloom reject
      if (is_subset(C.lits, D.lits)) {
        st.clauses[di].deleted = true;
        ndel++;
      }
    }
  }
  if (ndel > 0) {
    stat += ndel;
    st.rebuild_occ();
    return true;
  }
  return false;
}

// Pure-duplicate resolution: (ℓ ∨ K) ∧ (¬ℓ ∨ K) → (K) with K
// bit-identical between the two originals. Returns true iff at least
// one pair was resolved. May queue forced units for the caller to BCP.
static bool pass_pure_duplicate(State &st, unsigned long &stat) {
  unsigned n = 0;
  for (unsigned ci = 0; ci < st.clauses.size(); ci++) {
    if (st.clauses[ci].deleted) continue;
    auto &C = st.clauses[ci];
    bool ci_consumed = false;
    for (size_t j = 0; j < C.lits.size() && !ci_consumed; j++) {
      int ell = C.lits[j];
      unsigned negkey = lit_key(-ell);
      for (unsigned di : st.occ[negkey]) {
        if (di <= ci) continue;
        if (st.clauses[di].deleted) continue;
        const auto &D = st.clauses[di];
        if (D.lits.size() != C.lits.size()) continue;
        // Compare C \ {ell} vs D \ {-ell}.
        std::vector<int> Cminus, Dminus;
        Cminus.reserve(C.lits.size() - 1);
        Dminus.reserve(D.lits.size() - 1);
        for (int x : C.lits) if (x != ell) Cminus.push_back(x);
        for (int x : D.lits) if (x != -ell) Dminus.push_back(x);
        if (Cminus != Dminus) continue;

        // Apply the resolution.
        if (Cminus.empty()) {
          // (ℓ) ∧ (¬ℓ) — formula is UNSAT.
          st.unsat = true;
          return true;
        }
        if (Cminus.size() == 1) {
          // Resolvent is a unit. Force + mark both originals deleted.
          int u = Cminus[0];
          st.clauses[ci].deleted = true;
          st.clauses[di].deleted = true;
          st.force(u);
          n++;
          ci_consumed = true;
          break;
        }
        C.lits = std::move(Cminus);
        C.sig = sig_of(C.lits);
        st.clauses[di].deleted = true;
        n++;
        ci_consumed = true;   // C changed; next ci
        break;
      }
    }
  }
  if (n > 0) {
    stat += n;
    return true;
  }
  return false;
}

// Self-subsuming resolution: for clauses C and D, if resolving them
// on some literal yields R ⊆ D, replace D with R. Equivalently: D has
// some literal m = -ℓ, there exists C containing ℓ with C \ {ℓ} ⊆
// D \ {m}, then D := D \ {m}.
//
// Returns true iff at least one clause was shortened (or a unit was
// forced by a shortening that collapsed D to size 1).
static bool pass_ssr(State &st, unsigned long &stat) {
  unsigned n = 0;
  for (unsigned di = 0; di < st.clauses.size(); di++) {
    if (st.clauses[di].deleted) continue;
    auto &D = st.clauses[di];
    size_t k = 0;
    while (k < D.lits.size()) {
      int m = D.lits[k];
      int ell = -m;
      unsigned ellkey = lit_key(ell);
      bool dropped = false;
      for (unsigned ci : st.occ[ellkey]) {
        if (ci == di) continue;
        if (st.clauses[ci].deleted) continue;
        const auto &C = st.clauses[ci];
        if (C.lits.size() > D.lits.size()) continue;
        // Bloom reject: C \ {ell}'s bits must be a subset of D's bits.
        uint64_t need = C.sig & ~bloom_bit(ell);
        if ((need & ~D.sig) != 0) continue;
        // Construct C \ {ell} and D \ {m} and test subset.
        std::vector<int> Cminus;
        Cminus.reserve(C.lits.size() - 1);
        for (int x : C.lits) if (x != ell) Cminus.push_back(x);
        std::vector<int> Dminus;
        Dminus.reserve(D.lits.size() - 1);
        for (size_t p = 0; p < D.lits.size(); p++)
          if (p != k) Dminus.push_back(D.lits[p]);
        if (!is_subset(Cminus, Dminus)) continue;
        // Apply.
        if (Dminus.empty()) {
          // D had exactly one literal after dropping m, and now 0 —
          // UNSAT at F.
          st.unsat = true;
          return true;
        }
        if (Dminus.size() == 1) {
          int u = Dminus[0];
          st.clauses[di].deleted = true;
          st.force(u);
          n++;
          dropped = true;
          break;
        }
        D.lits = std::move(Dminus);
        D.sig = sig_of(D.lits);
        n++;
        dropped = true;
        break;
      }
      if (!dropped) {
        ++k;
      } else if (st.clauses[di].deleted) {
        break;    // D was consumed by force → next clause
      } else {
        // D shortened; don't advance k because the new D[k] may match
        // another C. Fine to leave k where it is and retry.
      }
    }
  }
  if (n > 0) {
    stat += n;
    return true;
  }
  return false;
}

}  // namespace

// -------------------------------------------------------------------
// Public entry point.
// -------------------------------------------------------------------

PreprocessorResult preprocess(
    unsigned n_vars,
    const std::vector<std::vector<int>> &in_clauses,
    const PreprocessorConfig &cfg)
{
  using namespace std::chrono;
  auto t0 = steady_clock::now();

  PreprocessorResult out;
  out.num_vars = n_vars;

  State st;
  st.n_vars = n_vars;
  st.val.assign(n_vars + 2, 0);

  // Ingest input clauses: dedupe literals, skip tautologies, pull out
  // unit and empty clauses.
  auto budget_left = [&]() {
    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    return elapsed < (long long)cfg.time_budget_ms;
  };

  for (const auto &raw : in_clauses) {
    std::vector<int> lits = raw;
    std::sort(lits.begin(), lits.end());
    lits.erase(std::unique(lits.begin(), lits.end()), lits.end());
    bool taut = false;
    for (size_t i = 1; i < lits.size() && !taut; i++)
      if (lits[i] == -lits[i-1]) taut = true;
    if (taut) continue;
    if (lits.empty()) { out.unsat = true; break; }
    if (lits.size() == 1) {
      if (!st.force(lits[0])) { out.unsat = true; break; }
      continue;
    }
    InternalClause c;
    c.lits = std::move(lits);
    c.sig = sig_of(c.lits);
    st.clauses.push_back(std::move(c));
  }

  if (!out.unsat) {
    st.rebuild_occ();
    // Initial BCP on any input units.
    if (!st.bcp()) out.unsat = true;
  }

  // Main fixpoint loop.
  while (!out.unsat && budget_left()) {
    bool changed = false;
    out.passes++;

    if (cfg.enable_subsumption && budget_left()) {
      if (pass_subsumption(st, out.num_subsumed)) changed = true;
      if (st.unsat) { out.unsat = true; break; }
    }
    if (cfg.enable_pure_duplicate && budget_left()) {
      if (pass_pure_duplicate(st, out.num_pure_dup)) {
        changed = true;
        if (!st.bcp()) { out.unsat = true; break; }
      }
      if (st.unsat) { out.unsat = true; break; }
    }
    if (cfg.enable_ssr && budget_left()) {
      if (pass_ssr(st, out.num_ssr)) {
        changed = true;
        if (!st.bcp()) { out.unsat = true; break; }
      }
      if (st.unsat) { out.unsat = true; break; }
    }

    if (cfg.verbose) {
      unsigned live = 0;
      for (const auto &c : st.clauses) if (!c.deleted) live++;
      std::cerr << "preproc pass=" << out.passes
                << " subsumed=" << out.num_subsumed
                << " pure_dup=" << out.num_pure_dup
                << " ssr=" << out.num_ssr
                << " forced=" << st.forced.size()
                << " live=" << live
                << " elapsed_ms="
                << duration_cast<milliseconds>(steady_clock::now() - t0).count()
                << "\n";
    }

    if (!changed) break;
  }

  out.elapsed_ms = duration_cast<milliseconds>(
      steady_clock::now() - t0).count();

  if (out.unsat) return out;

  // Collect surviving clauses (each already sorted ascending).
  for (const auto &c : st.clauses) {
    if (c.deleted) continue;
    // Guard: every surviving clause must have size >= 2. Size 1
    // should have been consumed via force(); size 0 indicates a
    // missed UNSAT detection — abort loudly rather than pass a
    // malformed CNF downstream.
    if (c.lits.size() < 2) {
      std::cerr << "\n*** PREPROCESSOR_MALFORMED_SURVIVOR ***\n"
                << "  lits.size()=" << c.lits.size() << "\n";
      std::cerr.flush();
      std::abort();
    }
    out.clauses.push_back(c.lits);
  }
  out.forced_units = std::move(st.forced);
  out.num_bcp_units = out.forced_units.size();

  return out;
}
