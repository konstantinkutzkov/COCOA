/*
 * structures.h
 *
 *  Created on: Jun 25, 2012
 *      Author: Marc Thurley
 */

#ifndef STRUCTURES_H_
#define STRUCTURES_H_

#include <vector>
#include <iostream>
#include <cstdint>
#include "primitive_types.h"
using namespace std;



#define INVALID_DL -1

typedef unsigned char TriValue;
#define   F_TRI  0
#define   T_TRI  1
#define   X_TRI  2

class LiteralID {
public:

  LiteralID() {
    value_ = 0;
  }
  LiteralID(int lit) {
    value_ = (abs(lit) << 1) + (unsigned) (lit > 0);
  }

  LiteralID(VariableIndex var, bool sign) {
    value_ = (var << 1) + (unsigned) sign;
  }

  VariableIndex var() const {
    return (value_ >> 1);
  }

  int toInt() const {
    return ((int) value_ >> 1) * ((sign()) ? 1 : -1);
  }

  void inc(){++value_;}

  void copyRaw(unsigned int v) {
    value_ = v;
  }

  bool sign() const {
    return (bool) (value_ & 0x01);
  }

  bool operator!=(const LiteralID &rL2) const {
    return value_ != rL2.value_;
  }

  bool operator==(const LiteralID &rL2) const {
    return value_ == rL2.value_;
  }

  const LiteralID neg() const {
    return LiteralID(var(), !sign());
  }

  void print() const {
    cout << (sign() ? " " : "-") << var() << " ";
  }

  unsigned raw() const { return value_;}

private:
  unsigned value_;

  template <class _T> friend class LiteralIndexedVector;
};

static const LiteralID NOT_A_LIT(0, false);
#define SENTINEL_LIT NOT_A_LIT

// Watch-list entry with cached "blocker" literal (Glucose/MiniSat-2 style).
// On BCP entry, if literal_values_[blocker] == T, the clause is satisfied
// without touching the clause body — saves the cache-line miss to beginOf(ofs)
// for the dominant "B path" (52% of BCP visits under triple_no_lockstep).
// Sentinel: WatchEntry() has ofs=SENTINEL_CL, blocker=NOT_A_LIT.
struct WatchEntry {
  ClauseOfs ofs;
  LiteralID blocker;
  WatchEntry() : ofs(SENTINEL_CL), blocker(NOT_A_LIT) {}
  WatchEntry(ClauseOfs o, LiteralID b) : ofs(o), blocker(b) {}
};

class Literal {
public:
  vector<LiteralID> binary_links_ = vector<LiteralID>(1,SENTINEL_LIT);
  // Parallel to binary_links_ for the ORIGINAL portion
  // [0, original_binary_link_count_): stores the global binary-clause id
  // used by Instance::deriv_cache_binary_clause_content_hash_ and
  // deriv_cache_binary_clause_sat_count_. Populated by
  // deriv_cache_track_init_; left empty when derivative-cache flags are off.
  vector<unsigned> binary_link_ids_;
  // Global-redundant binary lane. Holds binaries that are sound
  // consequences of the *original* CNF (e.g. SCC equivalences extracted
  // by preprocessing). BCP enforces these for propagation, but the
  // component analyzer and canonical-key builder DO NOT walk this lane —
  // so injecting them never bridges previously-independent components
  // and never changes the cache key of a sub-component. Sound because
  // any such binary is implied by the originals, hence implied by any
  // sub-formula whose vars include both of the binary's literals.
  vector<LiteralID> redundant_binary_links_ = vector<LiteralID>(1,SENTINEL_LIT);
  vector<WatchEntry> watch_list_ = vector<WatchEntry>(1, WatchEntry());
  float activity_score_ = 0.0f;

  // Number of original (non-learned) binary links.
  // Set after preprocessing; learned links are appended after this.
  unsigned original_binary_link_count_ = 0;

  void recordOriginalBinaryLinks() {
    // binary_links_ has SENTINEL_LIT at the end, so count = size - 1
    original_binary_link_count_ = binary_links_.size() - 1;
  }

  void removeLearnedBinaryLinks() {
    if (binary_links_.size() > original_binary_link_count_ + 1) {
      binary_links_.resize(original_binary_link_count_ + 1);  // keep sentinel
    }
  }

  void increaseActivity(unsigned u = 1){
    activity_score_+= u;
  }

  void removeWatchLinkTo(ClauseOfs clause_ofs) {
    for (auto it = watch_list_.begin(); it != watch_list_.end(); it++)
          if (it->ofs == clause_ofs) {
            *it = watch_list_.back();
            watch_list_.pop_back();
            return;
          }
  }

  // Replace ofs only; blocker is unchanged (the clause's literal set hasn't
  // moved, just its address in the lit pool).
  void replaceWatchLinkTo(ClauseOfs clause_ofs, ClauseOfs replace_ofs) {
        for (auto it = watch_list_.begin(); it != watch_list_.end(); it++)
          if (it->ofs == clause_ofs) {
            it->ofs = replace_ofs;
            return;
          }
  }

  // Caller must pass the OTHER watched literal of the clause as the blocker.
  void addWatchLinkTo(ClauseIndex clause_ofs, LiteralID blocker) {
    watch_list_.push_back(WatchEntry(clause_ofs, blocker));
  }

  void addBinLinkTo(LiteralID lit) {
    binary_links_.back() = lit;
    binary_links_.push_back(SENTINEL_LIT);
  }

  // Append to the redundant lane. Same sentinel-terminated convention so
  // BCP can use the same loop idiom (iterate until SENTINEL_LIT).
  void addRedundantBinLinkTo(LiteralID lit) {
    redundant_binary_links_.back() = lit;
    redundant_binary_links_.push_back(SENTINEL_LIT);
  }

  void resetWatchList(){
        watch_list_.clear();
        watch_list_.push_back(WatchEntry());
  }

  bool hasBinaryLinkTo(LiteralID lit) {
    for (auto l : binary_links_) {
      if (l == lit)
        return true;
    }
    return false;
  }

  bool hasBinaryLinks() {
    return !binary_links_.empty();
  }
};

class Antecedent {
  unsigned int val_;

public:
  Antecedent() {
    val_ = 1;
  }

  Antecedent(const ClauseOfs cl_ofs) {
     val_ = (cl_ofs << 1) | 1;
   }
  Antecedent(const LiteralID idLit) {
    val_ = (idLit.raw() << 1);
  }

  bool isAClause() const {
    return val_ & 0x01;
  }

  ClauseOfs asCl() const {
      return val_ >> 1;
    }

  LiteralID asLit() {
    LiteralID idLit;
    idLit.copyRaw(val_ >> 1);
    return idLit;
  }
  // A NON-Antecedent will only be A NOT_A_CLAUSE Clause Id
  bool isAnt() {
    return val_ != 1; //i.e. NOT a NOT_A_CLAUSE;
  }
};


struct Variable {
  Antecedent ante;
  int decision_level = INVALID_DL;
  // NOTE: the "is_branch_constraint" flag (set by branchOnClause's
  // negate arm for ¬l_2..¬l_k) lives in Instance::is_branch_constraint_
  // (a parallel std::vector<bool>), NOT here. Keeping Variable at 8
  // bytes preserves cache-line density on the hot variables_[v] access
  // path. See docs/branchonclause_branch_constraint_plan.md.
};

// for now Clause Header is just a dummy
// we keep it for possible later changes
class ClauseHeader {
  unsigned creation_time_; // number of conflicts seen at creation time
  unsigned score_;
  unsigned length_;
public:

  void increaseScore() {
    score_++;
  }
  void decayScore() {
      score_ >>= 1;
  }
  unsigned score() {
      return score_;
  }

  unsigned creation_time() {
      return creation_time_;
  }
  unsigned length(){ return length_;}
  void set_length(unsigned length){ length_ = length;}

  void set_creation_time(unsigned time) {
    creation_time_ = time;
  }
  static unsigned overheadInLits(){return sizeof(ClauseHeader)/sizeof(LiteralID);}
};

#endif /* STRUCTURES_H_ */
