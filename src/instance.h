/*
 * instance.h
 *
 *  Created on: Aug 23, 2012
 *      Author: Marc Thurley
 */

#ifndef INSTANCE_H_
#define INSTANCE_H_

#include "statistics.h"
#include "structures.h"
#include "containers.h"

#include <assert.h>
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <set>
#include <unordered_map>
#include <unordered_set>

class Instance {
protected:

  void unSet(LiteralID lit) {
    // Guard 2: the guard variable must NEVER be unset. Its T_TRI
    // assignment at DL 0 is the invariant that makes binary-UIP padding
    // sound. Violation would make the padding literal ¬d become free,
    // and padded clauses would fire (or fail to fire) incorrectly,
    // silently undercounting or overcounting. Runtime check — fires
    // even in Release so a bug can't sneak through.
    if (guard_var_ != 0 && lit.var() == guard_var_) {
      std::cerr << "*** GUARD_VAR_UNSET ***\n"
                << "  guard_var_=" << guard_var_
                << " lit.var()=" << lit.var()
                << " — guard must stay assigned at DL 0; see instance.h.\n";
      std::cerr.flush();
      std::abort();
    }
    var(lit).ante = Antecedent(NOT_A_CLAUSE);
    var(lit).decision_level = INVALID_DL;
    literal_values_[lit] = X_TRI;
    literal_values_[lit.neg()] = X_TRI;
    // Invariant R7: unSet must clear the antecedent so the variable
    // looks "decision-like" (no antecedent) on subsequent re-set.
    // hasAntecedent must now report false. Always-on (one bool check
    // per unSet — cheap).
    if (variables_[lit.var()].ante.isAnt()) {
      std::cerr << "\n*** INV_R7_UNSET_DID_NOT_CLEAR_ANTE ***\n"
                << "  lit=" << lit.toInt() << "\n";
      std::cerr.flush();
      std::abort();
    }
  }

  Antecedent & getAntecedent(LiteralID lit) {
    return variables_[lit.var()].ante;
  }

  bool hasAntecedent(LiteralID lit) {
    return variables_[lit.var()].ante.isAnt();
  }

  bool isAntecedentOf(ClauseOfs ante_cl, LiteralID lit) {
    return var(lit).ante.isAClause() && (var(lit).ante.asCl() == ante_cl);
  }

  bool isolated(VariableIndex v) {
    LiteralID lit(v, false);
    return (literal(lit).binary_links_.size() <= 1)
        & occurrence_lists_[lit].empty()
        & (literal(lit.neg()).binary_links_.size() <= 1)
        & occurrence_lists_[lit.neg()].empty();
  }

  bool free(VariableIndex v) {
    return isolated(v) & isActive(v);
  }

  bool deleteConflictClauses();
  void deleteAllConflictClauses();
  bool markClauseDeleted(ClauseOfs cl_ofs);

  // Compact the literal pool erasing all the clause
  // information from deleted clauses
  void compactConflictLiteralPool();

  // we assert that the formula is consistent
  // and has not been found UNSAT yet
  // hard wires all assertions in the literal stack into the formula
  // removes all set variables and essentially reinitiallizes all
  // further data
  void compactClauses();
  void compactVariables();
  void cleanClause(ClauseOfs cl_ofs);

  /////////////////////////////////////////////////////////
  // END access to variables and literals
  /////////////////////////////////////////////////////////


  unsigned int num_conflict_clauses() const {
    return conflict_clauses_.size();
  }

  unsigned int num_variables() {
    return variables_.size() - 1;
  }

  bool createfromFile(const string &file_name);

  DataAndStatistics statistics_;

  /** literal_pool_: the literals of all clauses are stored here
   *   INVARIANT: first and last entries of literal_pool_ are a SENTINEL_LIT
   *
   *   Clauses begin with a ClauseHeader structure followed by the literals
   *   terminated by SENTINEL_LIT
   */
  vector<LiteralID> literal_pool_;

  // this is to determine the starting offset of
  // conflict clauses
  unsigned original_lit_pool_size_;

  // Set by createfromFile when the input CNF contains an empty clause
  // (a "0" line with no literals). Such a formula is trivially UNSAT;
  // the count is 0 regardless of any other clauses. Checked at the top
  // of simplePreProcess so the UNSAT short-circuit fires even with
  // -noPP. Without this, the parser silently dropped the empty clause
  // (the `assert(!literals.empty())` is compiled out in Release builds)
  // and the solver returned a non-zero count.
  bool parsed_trivially_unsat_ = false;

  LiteralIndexedVector<Literal> literals_;

  LiteralIndexedVector<vector<ClauseOfs> > occurrence_lists_;

  vector<ClauseOfs> conflict_clauses_;
  vector<LiteralID> unit_clauses_;

  // Per-component BCP filter state:
  //   current_sub_varset_  : bitmap (1-indexed by var ID), 1 if var is in
  //                          the CURRENTLY-SOLVING sub-component's
  //                          varsBegin. Empty = no filtering (root state).
  //   current_sub_var_list_: sorted list of vars currently set in the bitmap.
  //                          Used by SubVarsetGuard to compute the
  //                          parent/child diff in O(|parent|+|child|)
  //                          rather than O(num_vars) per push/pop.
  // Maintained by Solver::solveComponent's SubVarsetGuard RAII helper.
  std::vector<char> current_sub_varset_;
  std::vector<unsigned> current_sub_var_list_;

  vector<Variable> variables_;
  LiteralIndexedVector<TriValue> literal_values_;

  // Guard variable for scope-tracked binary UIP learning.
  //
  // Background: learned clauses need a ClauseOfs to key their scope in
  // learned_clause_scope_. Binaries stored in Literal::binary_links_ have
  // no ClauseOfs, and binary BCP walks binary_links_ with no scope gate.
  // Storing a binary UIP there under non-empty scope would be unsound.
  //
  // Solution: allocate one guard variable d post-preprocessing, assign
  // d=true at decision level 0, and represent every learned binary
  // (y v z) as the scoped 3-clause [y, z, ¬d] in literal_pool_. Since
  // ¬d is permanently F_TRI, the clause behaves as a binary via the
  // watched-literal mechanism, but now has a ClauseOfs and scope entry.
  //
  // INVARIANT (load-bearing for model counting): d must remain non-active
  // at all times. literal_values_[d.pos] = T_TRI is set once and never
  // cleared. Any code that treats isolated variables as free must also
  // check isActive; free(v) in this file already does. Do NOT allocate
  // d before preprocessing — compactVariables would eliminate it and
  // double the model count via num_free_variables_.
  //
  // guard_var_ == 0 means "not yet allocated"; binary UIP padding is
  // then skipped (falls back to pre-guard behaviour: drop the binary).
  unsigned guard_var_ = 0;

  // Inverse of compactVariables()'s var_map: compact_to_orig_[c] gives the
  // original variable number that compacted var c refers to. Populated at
  // the end of compactVariables(). Empty if compactVariables has not run.
  std::vector<unsigned> compact_to_orig_;

  // ------------------------------------------------------------------
  // Learned-clause deduplication via Bloom filter.
  //
  // Rationale: measurement showed ~50% of learned clauses (from the
  // conflict-UIP path) are exact literal-set duplicates of existing
  // clauses. The BCP pool inflation from those duplicates dominates
  // the cost of learning.
  //
  // Bloom filter is ideal here: we don't need exact dedup (a false
  // positive just means we skip learning a clause — sound, just an
  // optimization loss). Fixed memory. No heap churn after construction.
  //
  // 2^23 bits = 1 MB, 4 hash functions via double hashing. With 100K
  // unique items, FP rate < 1e-5; still <1% at 1M items. For typical
  // MC2025 solves we'll be well below 100K.
  //
  // Signature mixes the clause's literals AND the current scope
  // (removed_clauses_ keys, sorted for determinism), so identical
  // literals under different clause-branching scopes get different
  // signatures. Scope-compatible duplicates are correctly merged;
  // scope-incompatible copies are both kept.
  // ------------------------------------------------------------------
  class ClauseSigFilter {
  public:
    static constexpr size_t kBits = size_t(1) << 23;   // 1 MB
    static constexpr size_t kMask = kBits - 1;
    static constexpr int kK = 4;

    ClauseSigFilter() : bits_((kBits + 63) / 64, 0ULL) {}

    // Returns true if `sig`'s bits were not all already set —
    // i.e., this is (very likely) a new signature. Returns false if
    // all bits were already set (likely duplicate).
    bool insert(uint64_t sig) {
      // Double hashing: derive k positions from two independent 64-bit
      // hashes of sig.
      uint64_t h1 = sig * 0x9E3779B97F4A7C15ULL;
      uint64_t h2 = sig * 0xBF58476D1CE4E5B9ULL;
      bool is_new = false;
      for (int k = 0; k < kK; k++) {
        size_t idx = ((h1 + (uint64_t)k * h2) >> 32) & kMask;
        size_t w = idx >> 6, b = idx & 63;
        uint64_t mask = (uint64_t)1 << b;
        if (!(bits_[w] & mask)) {
          bits_[w] |= mask;
          is_new = true;
        }
      }
      return is_new;
    }

  private:
    std::vector<uint64_t> bits_;
  };

  ClauseSigFilter learned_clause_sig_;

  // Returns true iff the clause is (very likely) new. Caller should
  // then proceed to addClause / addScopedUIPConflictClause. On false,
  // caller should skip the learn. Scope (removed_clauses_ keys) is
  // mixed into the signature; scope-incompatible copies with same
  // literals are distinguished.
  bool maybeDedupClause(const std::vector<LiteralID> &clause) {
    // Static (NOT thread_local) buffer to avoid both per-call heap alloc
    // AND per-access _tlv_get_addr overhead. See canonical_key.cpp's
    // top-of-file note: solver is single-threaded; if that ever changes,
    // revert to thread_local to avoid data races.
    static std::vector<uint32_t> sig_buf;
    sig_buf.clear();
    sig_buf.reserve(clause.size());
    for (auto l : clause) sig_buf.push_back(l.raw());
    std::sort(sig_buf.begin(), sig_buf.end());

    // FNV-1a over sorted literal raws, then over sorted scope keys.
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t r : sig_buf) { h ^= r; h *= 0x100000001B3ULL; }

    // removed_clauses_ is an unordered_map — iteration order is not
    // deterministic, so collect keys and sort.
    if (!removed_clauses_.empty()) {
      static std::vector<ClauseOfs> scope_buf;  // single-threaded; see canonical_key.cpp
      scope_buf.clear();
      scope_buf.reserve(removed_clauses_.size());
      for (const auto &p : removed_clauses_) scope_buf.push_back(p.first);
      std::sort(scope_buf.begin(), scope_buf.end());
      h ^= 0xDEADBEEFCAFEBABEULL;  // separator between literals and scope
      for (ClauseOfs c : scope_buf) {
        h ^= (uint64_t)c;
        h *= 0x100000001B3ULL;
      }
    }

    return learned_clause_sig_.insert(h);
  }

  // Allocate the guard variable. Call ONCE after simplePreProcess
  // (HardWireAndCompact) has finished, before countSATRec.
  void allocateGuardVariable() {
    assert(guard_var_ == 0 && "guard variable already allocated");
    guard_var_ = variables_.size();  // new 1-based index = current size
    variables_.push_back(Variable{});
    literals_.resize(variables_.size());
    literal_values_.resize(variables_.size(), X_TRI);
    occurrence_lists_.resize(variables_.size());
    LiteralID d_pos(guard_var_, true);
    literal_values_[d_pos] = T_TRI;
    literal_values_[d_pos.neg()] = F_TRI;
    variables_[guard_var_].ante = Antecedent(NOT_A_CLAUSE);
    variables_[guard_var_].decision_level = 0;
    // Guard 1: position invariant — guard_var_ must equal num_variables()
    // so padded clauses use the correct index and future allocations
    // (if any) would corrupt the invariant.
    assert(guard_var_ == num_variables()
           && "guard variable must be the last variable index");
    // Guard 6: isolation at allocation — the guard must not already
    // appear in any clause or binary link. If this ever fires, someone
    // moved allocateGuardVariable earlier (into preprocessing) or the
    // resize didn't zero-initialize the new per-literal slots.
    assert(literal(d_pos).binary_links_.size() == 1
           && literal(d_pos.neg()).binary_links_.size() == 1
           && occurrence_lists_[d_pos].empty()
           && occurrence_lists_[d_pos.neg()].empty()
           && "guard variable must be isolated at allocation");
  }

  // Clauses removed by clause branching — reference counted
  // because the same clause can be branched on at multiple levels.
  // Handled like satisfied clauses: stay in watch lists, skipped by BCP.
  std::unordered_map<ClauseOfs, unsigned> removed_clauses_;

  // Scope of each learned clause: the set of clauses that were removed
  // (via clause branching) at the moment this clause was learned.
  //
  // Soundness: let S_learn = scope stored at learn time and S_use =
  // removed_clauses_ at use time. D was derived from F\S_learn, so
  // F\S_learn ⊨ D. The current formula F\S_use entails D iff
  //   S_use ⊆ S_learn,
  // because then F\S_use ⊇ F\S_learn (use has more constraints),
  // MODELS(F\S_use) ⊆ MODELS(F\S_learn), and every model of F\S_use
  // satisfies D.
  //
  // Only tracked for non-binary learned clauses (binary ones have no
  // ClauseOfs). Clauses with no entry in the map are assumed to have
  // been learned at empty scope (sound only when S_use is also empty).
  std::unordered_map<ClauseOfs, std::set<ClauseOfs>> learned_clause_scope_;

  // True iff the learned clause at cl_ofs is sound under the current
  // removed_clauses_. Requires current_removed ⊆ stored_scope.
  // Non-learned clauses (ofs < original_lit_pool_size_) are always
  // sound; caller should check that first.
  bool learnedClauseInScope(ClauseOfs cl_ofs) const {
    auto it = learned_clause_scope_.find(cl_ofs);
    // Treat "no entry" as scope = empty set; then we need
    // current_removed ⊆ ∅, i.e. current_removed is empty.
    if (it == learned_clause_scope_.end())
      return removed_clauses_.empty();
    const auto &scope = it->second;
    // Every currently-removed clause must be in scope.
    for (const auto &p : removed_clauses_) {
      if (scope.count(p.first) == 0)
        return false;
    }
    return true;
  }

  // True iff the learned clause at cl_ofs has no variable that is
  // BOTH outside `mask` (the current sub-component's varsBegin) AND
  // still currently active (X_TRI). Such a variable would belong to
  // a sibling sub-component, and BCP on this clause inside the
  // current sub could force its value — cross-sub propagation that
  // contaminates the cached count.
  //
  // Vars outside `mask` that are already assigned (T_TRI/F_TRI by
  // parent decisions) are FINE to keep: their lit in the clause is
  // either satisfying the clause (treated as removed) or falsified
  // (clause is effectively shorter), and BCP handles both cases
  // soundly via the existing isResolved/isSatisfied checks.
  //
  // guard_var_ is the binary-padding sentinel (permanently F_TRI),
  // never represents a real formula variable, always skipped.
  // Non-learned clauses are always in-scope (caller filters by
  // ofs < original_lit_pool_size_ first).
  bool learnedClauseInComponent(ClauseOfs cl_ofs,
                                const std::vector<char> &mask) const {
    if (mask.empty()) return true;  // no current sub set → no filtering
    for (auto lt = literal_pool_.begin() + cl_ofs; *lt != SENTINEL_LIT; lt++) {
      unsigned v = lt->var();
      if (v == guard_var_) continue;
      if (v < mask.size() && mask[v]) continue;  // in current sub
      // Outside the current sub. Only block when still active
      // (X_TRI) — an active outside var means cross-sub hazard.
      if (literal_values_[LiteralID(v, true)] == X_TRI) return false;
    }
    return true;
  }

  bool isClauseRemoved(ClauseOfs cl_ofs) const {
    return removed_clauses_.count(cl_ofs) > 0;
  }

  void markClauseRemoved(ClauseOfs cl_ofs) {
    removed_clauses_[cl_ofs]++;
  }

  void unmarkClauseRemoved(ClauseOfs cl_ofs) {
    auto it = removed_clauses_.find(cl_ofs);
    if (it != removed_clauses_.end()) {
      if (--it->second == 0)
        removed_clauses_.erase(it);
    }
  }

  void decayActivities() {
    for (auto l_it = literals_.begin(); l_it != literals_.end(); l_it++)
      l_it->activity_score_ *= 0.5;

    for(auto clause_ofs: conflict_clauses_)
        getHeaderOf(clause_ofs).decayScore();

  }

  void updateActivities(ClauseOfs clause_ofs) {
    getHeaderOf(clause_ofs).increaseScore();
    for (auto it = beginOf(clause_ofs); *it != SENTINEL_LIT; it++) {
      literal(*it).increaseActivity();
    }
  }

  bool isUnitClause(const LiteralID lit) {
    for (auto l : unit_clauses_)
      if (l == lit)
        return true;
    return false;
  }

  bool existsUnitClauseOf(VariableIndex v) {
    for (auto l : unit_clauses_)
      if (l.var() == v)
        return true;
    return false;
  }

  // addUnitClause checks whether lit or lit.neg() is already a
  // unit clause
  // a negative return value implied that the Instance is UNSAT
  bool addUnitClause(const LiteralID lit) {
    for (auto l : unit_clauses_) {
      if (l == lit)
        return true;
      if (l == lit.neg())
        return false;
    }
    unit_clauses_.push_back(lit);
    return true;
  }

  inline ClauseIndex addClause(vector<LiteralID> &literals);

  // adds a UIP Conflict Clause
  // and returns it as an Antecedent to the first
  // literal stored in literals
  inline Antecedent addUIPConflictClause(vector<LiteralID> &literals);
  // Scoped variant: records the set of clauses currently in removed_clauses_
  // as the scope of the learned clause. BCP will skip this clause whenever
  // the scope is not a subset of the current removed_clauses_.
  //
  // If the learned clause would be binary (size < 3), this is a no-op: we
  // can't key scope onto binary clauses (they have no ClauseOfs). The caller
  // should check uip_clauses_.back().size() >= 3 if they want a scoped learn.
  //
  // `pad_binary`: when true and a guard_var_ is allocated, binary UIPs are
  // padded with ¬guard to give them a ClauseOfs. When false, binary UIPs
  // are dropped. Pass false to isolate the padding mechanism for bug hunting.
  //
  // `record_scope`: when true, records removed_clauses_ as the clause's
  // scope (default). When false, the clause is treated as unscoped — sound
  // only when removed_clauses_ stays empty across the clause's lifetime.
  inline Antecedent addScopedUIPConflictClause(vector<LiteralID> &literals,
                                                bool pad_binary = true,
                                                bool record_scope = true);

  inline bool addBinaryClause(LiteralID litA, LiteralID litB);

  // Inject an equivalence x = (y if same_polarity else ¬y) into the
  // redundant-binary lane. Sound only when the equivalence is implied
  // by the original CNF (e.g. detected by CMS5's SCC pass at
  // preprocessing time). Each equivalence corresponds to two binary
  // clauses, each stored at both endpoints' lanes (4 lane entries total).
  //
  // The redundant lane is BCP-visible (so the equivalence propagates)
  // and analyzer/cache-key-invisible (so it never bridges components
  // or alters cache keys). See Literal::redundant_binary_links_ for
  // soundness rationale.
  //
  // Returns true if at least one entry was added; false if the
  // equivalence was already redundant or if either variable is
  // out of range / inactive.
  inline bool addRedundantBinaryEquivalence(unsigned var_x, unsigned var_y,
                                             bool same_polarity);

  /////////////////////////////////////////////////////////
  // BEGIN access to variables, literals, clauses
  /////////////////////////////////////////////////////////

  inline Variable &var(const LiteralID lit) {
    return variables_[lit.var()];
  }

  Literal & literal(LiteralID lit) {
    return literals_[lit];
  }

  inline bool isSatisfied(const LiteralID &lit) const {
    return literal_values_[lit] == T_TRI;
  }

  bool isResolved(LiteralID lit) {
    return literal_values_[lit] == F_TRI;
  }

  bool isActive(LiteralID lit) const {
    return literal_values_[lit] == X_TRI;
  }

  vector<LiteralID>::const_iterator beginOf(ClauseOfs cl_ofs) const {
    return literal_pool_.begin() + cl_ofs;
  }
  vector<LiteralID>::iterator beginOf(ClauseOfs cl_ofs) {
    return literal_pool_.begin() + cl_ofs;
  }

  decltype(literal_pool_.begin()) conflict_clauses_begin() {
     return literal_pool_.begin() + original_lit_pool_size_;
   }

  ClauseHeader &getHeaderOf(ClauseOfs cl_ofs) {
    return *reinterpret_cast<ClauseHeader *>(&literal_pool_[cl_ofs
        - ClauseHeader::overheadInLits()]);
  }

  bool isSatisfied(ClauseOfs cl_ofs) {
    for (auto lt = beginOf(cl_ofs); *lt != SENTINEL_LIT; lt++)
      if (isSatisfied(*lt))
        return true;
    return false;
  }
};

ClauseIndex Instance::addClause(vector<LiteralID> &literals) {
  if (literals.size() == 1) {
    //TODO Deal properly with the situation that opposing unit clauses are learned
    assert(!isUnitClause(literals[0].neg()));
    unit_clauses_.push_back(literals[0]);
    return 0;
  }
  if (literals.size() == 2) {
    addBinaryClause(literals[0], literals[1]);
    return 0;
  }

  for (unsigned i = 0; i < ClauseHeader::overheadInLits(); i++)
    literal_pool_.push_back(0);
  ClauseOfs cl_ofs = literal_pool_.size();

  for (auto l : literals) {
    literal_pool_.push_back(l);
    literal(l).increaseActivity(1);
  }
  // make an end: SENTINEL_LIT
  literal_pool_.push_back(SENTINEL_LIT);
  literal(literals[0]).addWatchLinkTo(cl_ofs);
  literal(literals[1]).addWatchLinkTo(cl_ofs);
  getHeaderOf(cl_ofs).set_creation_time(statistics_.num_conflicts_);
  return cl_ofs;
}


Antecedent Instance::addUIPConflictClause(vector<LiteralID> &literals) {
    Antecedent ante(NOT_A_CLAUSE);
    statistics_.num_clauses_learned_++;
    ClauseOfs cl_ofs = addClause(literals);
    if (cl_ofs != 0) {
      conflict_clauses_.push_back(cl_ofs);
      getHeaderOf(cl_ofs).set_length(literals.size());
      ante = Antecedent(cl_ofs);
    } else if (literals.size() == 2){
      ante = Antecedent(literals.back());
      statistics_.num_binary_conflict_clauses_++;
    } else if (literals.size() == 1)
      statistics_.num_unit_clauses_++;
    return ante;
  }

Antecedent Instance::addScopedUIPConflictClause(vector<LiteralID> &literals,
                                                 bool pad_binary,
                                                 bool record_scope) {
    // Pad binary UIPs to 3-clauses using the guard literal (¬d,
    // permanently F_TRI). This gives them a ClauseOfs so scope can be
    // tracked and BCP can consult learnedClauseInScope. Without the
    // guard we would have to drop binary UIPs to stay sound — a real
    // learning loss. See the guard_var_ comment above.
    if (pad_binary && literals.size() == 2 && guard_var_ != 0) {
      literals.push_back(LiteralID(guard_var_, false));  // ¬d, always false
    }
    // Unit (size==1) still dropped — units live in unit_clauses_ and
    // only make sense at empty scope; scoped units would need a
    // different representation.
    if (literals.size() < 3)
      return Antecedent(NOT_A_CLAUSE);

    Antecedent ante(NOT_A_CLAUSE);
    statistics_.num_clauses_learned_++;
    ClauseOfs cl_ofs = addClause(literals);
    if (cl_ofs != 0) {
      conflict_clauses_.push_back(cl_ofs);
      getHeaderOf(cl_ofs).set_length(literals.size());
      ante = Antecedent(cl_ofs);
      // Record scope = current removed_clauses_ key set.
      if (record_scope && !removed_clauses_.empty()) {
        std::set<ClauseOfs> scope;
        for (const auto &p : removed_clauses_) scope.insert(p.first);
        // Invariant S1: scope size must equal removed_clauses_ size
        // (no entry should have been silently lost). Always-on check
        // since cost is ~zero (one comparison per learn call).
        if (scope.size() != removed_clauses_.size()) {
          std::cerr << "\n*** INV_S1_SCOPE_LOST_ENTRIES ***\n"
                    << "  cl_ofs=" << cl_ofs
                    << "  scope.size()=" << scope.size()
                    << "  removed_clauses_.size()="
                    << removed_clauses_.size() << "\n";
          std::cerr.flush();
          std::abort();
        }
        learned_clause_scope_.emplace(cl_ofs, std::move(scope));
      }
      // If removed_clauses_ is empty, scope is {} — unconditionally sound,
      // no entry needed in learned_clause_scope_.
    }
    return ante;
  }

bool Instance::addBinaryClause(LiteralID litA, LiteralID litB) {
   if (literal(litA).hasBinaryLinkTo(litB))
     return false;
   literal(litA).addBinLinkTo(litB);
   literal(litB).addBinLinkTo(litA);
   literal(litA).increaseActivity();
   literal(litB).increaseActivity();
   return true;
 }

bool Instance::addRedundantBinaryEquivalence(unsigned var_x, unsigned var_y,
                                              bool same_polarity) {
   // Storage convention (mirrors addBinaryClause): for a clause (a ∨ b),
   // BCP walks a's neg-lane when a becomes false to find that b is now
   // forced. addBinaryClause(a, b) stores b in a's lane and a in b's lane.
   //
   // x ↔ y encoded as (¬x ∨ y) ∧ (¬y ∨ x):
   //   (¬x ∨ y): store y_pos in x_neg-lane and x_neg in y_pos-lane.
   //   (¬y ∨ x): store x_pos in y_neg-lane and y_neg in x_pos-lane.
   //
   // x ↔ ¬y encoded as (¬x ∨ ¬y) ∧ (x ∨ y):
   //   (¬x ∨ ¬y): store y_neg in x_neg-lane and x_neg in y_neg-lane.
   //   (x ∨ y):   store y_pos in x_pos-lane and x_pos in y_pos-lane.
   if (var_x == 0 || var_y == 0 || var_x == var_y) return false;
   if (var_x >= variables_.size() || var_y >= variables_.size()) return false;

   LiteralID x_pos(var_x, false), x_neg(var_x, true);
   LiteralID y_pos(var_y, false), y_neg(var_y, true);

   bool added = false;
   auto add_one = [&](LiteralID side, LiteralID other) {
     auto &lane = literal(side).redundant_binary_links_;
     for (auto &e : lane) {
       if (e == other) return;
       if (e == SENTINEL_LIT) break;
     }
     literal(side).addRedundantBinLinkTo(other);
     added = true;
   };

   if (same_polarity) {
     add_one(x_neg, y_pos);  add_one(y_pos, x_neg);
     add_one(x_pos, y_neg);  add_one(y_neg, x_pos);
   } else {
     add_one(x_neg, y_neg);  add_one(y_neg, x_neg);
     add_one(x_pos, y_pos);  add_one(y_pos, x_pos);
   }
   return added;
 }


#endif /* INSTANCE_H_ */
