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
#include <set>
#include <unordered_map>
#include <unordered_set>

class Instance {
protected:

  void unSet(LiteralID lit) {
    var(lit).ante = Antecedent(NOT_A_CLAUSE);
    var(lit).decision_level = INVALID_DL;
    literal_values_[lit] = X_TRI;
    literal_values_[lit.neg()] = X_TRI;
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

  LiteralIndexedVector<Literal> literals_;

  LiteralIndexedVector<vector<ClauseOfs> > occurrence_lists_;

  vector<ClauseOfs> conflict_clauses_;
  vector<LiteralID> unit_clauses_;

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
  inline Antecedent addScopedUIPConflictClause(vector<LiteralID> &literals);

  inline bool addBinaryClause(LiteralID litA, LiteralID litB);

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

Antecedent Instance::addScopedUIPConflictClause(vector<LiteralID> &literals) {
    // Pad binary UIPs to 3-clauses using the guard literal (¬d,
    // permanently F_TRI). This gives them a ClauseOfs so scope can be
    // tracked and BCP can consult learnedClauseInScope. Without the
    // guard we would have to drop binary UIPs to stay sound — a real
    // learning loss. See the guard_var_ comment above.
    if (literals.size() == 2 && guard_var_ != 0) {
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
      if (!removed_clauses_.empty()) {
        std::set<ClauseOfs> scope;
        for (const auto &p : removed_clauses_) scope.insert(p.first);
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


#endif /* INSTANCE_H_ */
