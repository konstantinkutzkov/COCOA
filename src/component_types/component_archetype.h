/*
 * component_archetype.h
 *
 *  Created on: Feb 9, 2013
 *      Author: mthurley
 */

#ifndef COMPONENT_ARCHETYPE_H_
#define COMPONENT_ARCHETYPE_H_


#include "../primitive_types.h"
#include "component.h"


// cacheable_component.h removed — using content-based cache





#include <cstring>
#include <algorithm>



#include <iostream>
// State values for variables found during component
// analysis (CA)
typedef unsigned char CA_SearchState;
#define   CA_NIL  0
#define   CA_VAR_IN_SUP_COMP_UNSEEN  1
#define   CA_VAR_SEEN 2
#define   CA_VAR_IN_OTHER_COMP  4

#define   CA_VAR_MASK  7

#define   CA_CL_IN_SUP_COMP_UNSEEN  8
#define   CA_CL_SEEN 16
#define   CA_CL_IN_OTHER_COMP  32
#define   CA_CL_ALL_LITS_ACTIVE  64

#define   CA_CL_MASK  120

class StackLevel;

class ComponentArchetype {
public:
  ComponentArchetype() {
  }
  ComponentArchetype(StackLevel &stack_level, Component &super_comp) :
      p_super_comp_(&super_comp), p_stack_level_(&stack_level) {
  }

  void reInitialize(StackLevel &stack_level, Component &super_comp) {
    p_super_comp_ = &super_comp;
    p_stack_level_ = &stack_level;
    clearArrays();
    current_comp_for_caching_.reserveSpace(super_comp.num_variables(),super_comp.numLongClauses());
  }

  Component &super_comp() {
    return *p_super_comp_;
  }

  StackLevel & stack_level() {
    return *p_stack_level_;
  }

  void setVar_in_sup_comp_unseen(VariableIndex v) {
    seen_[v] = CA_VAR_IN_SUP_COMP_UNSEEN | (seen_[v] & CA_CL_MASK);
  }

  void setClause_in_sup_comp_unseen(ClauseIndex cl) {
    seen_[cl] = CA_CL_IN_SUP_COMP_UNSEEN | (seen_[cl] & CA_VAR_MASK);
  }

  void setVar_nil(VariableIndex v) {
    seen_[v] &= CA_CL_MASK;
  }

  void setClause_nil(ClauseIndex cl) {
    seen_[cl] &= CA_VAR_MASK;
  }

  void setVar_seen(VariableIndex v) {
    seen_[v] = CA_VAR_SEEN | (seen_[v] & CA_CL_MASK);
  }

  void setClause_seen(ClauseIndex cl) {
    setClause_nil(cl);
    seen_[cl] = CA_CL_SEEN | (seen_[cl] & CA_VAR_MASK);
  }

  void setClause_seen(ClauseIndex cl, bool all_lits_act) {
      setClause_nil(cl);
      seen_[cl] = CA_CL_SEEN | (all_lits_act?CA_CL_ALL_LITS_ACTIVE:0) | (seen_[cl] & CA_VAR_MASK);
    }

  void setVar_in_other_comp(VariableIndex v) {
    seen_[v] = CA_VAR_IN_OTHER_COMP | (seen_[v] & CA_CL_MASK);
  }

  void setClause_in_other_comp(ClauseIndex cl) {
    seen_[cl] = CA_CL_IN_OTHER_COMP | (seen_[cl] & CA_VAR_MASK);
  }

  bool var_seen(VariableIndex v) {
    return seen_[v] & CA_VAR_SEEN;
  }

  bool clause_seen(ClauseIndex cl) {
    return seen_[cl] & CA_CL_SEEN;
  }

  bool clause_all_lits_active(ClauseIndex cl) {
    return seen_[cl] & CA_CL_ALL_LITS_ACTIVE;
  }
  void setClause_all_lits_active(ClauseIndex cl) {
    seen_[cl] |= CA_CL_ALL_LITS_ACTIVE;
  }

  bool var_nil(VariableIndex v) {
    return (seen_[v] & CA_VAR_MASK) == 0;
  }

  bool clause_nil(ClauseIndex cl) {
    return (seen_[cl] & CA_CL_MASK) == 0;
  }

  bool var_unseen_in_sup_comp(VariableIndex v) {
    return seen_[v] & CA_VAR_IN_SUP_COMP_UNSEEN;
  }

  bool clause_unseen_in_sup_comp(ClauseIndex cl) {
    return seen_[cl] & CA_CL_IN_SUP_COMP_UNSEEN;
  }

  bool var_seen_in_peer_comp(VariableIndex v) {
    return seen_[v] & CA_VAR_IN_OTHER_COMP;
  }

  bool clause_seen_in_peer_comp(ClauseIndex cl) {
    return seen_[cl] & CA_CL_IN_OTHER_COMP;
  }

  static void initArrays(unsigned max_variable_id, unsigned max_clause_id) {
    unsigned seen_size = std::max(max_variable_id,max_clause_id)  + 1;
    seen_ = new CA_SearchState[seen_size];
    seen_byte_size_ = sizeof(CA_SearchState) * (seen_size);
    clearArrays();

  }

  static void clearArrays() {
    memset(seen_, CA_NIL, seen_byte_size_);
  }


  Component *makeComponentFromState(unsigned stack_size) {
    Component *p_new_comp = new Component();
    p_new_comp->reserveSpace(stack_size, super_comp().numLongClauses());
    current_comp_for_caching_.clear();

    // Compute the 128-bit L1 hash in the same iteration pass that
    // builds the Component. Two independent halves — `lo` uses the
    // original Murmur finalizer; `hi` uses fresh constants so it is
    // statistically independent. Together they give a 128-bit key
    // whose birthday-collision probability at our cache scale is
    // ~10^-20 — functionally zero — eliminating the need for a
    // structural equality check at L1 lookup time.
    auto mix_lo = [](uint64_t x) {
      x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
      x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
      x ^= x >> 33;
      return x;
    };
    auto mix_hi = [](uint64_t x) {
      // Different constants — e.g., the xxHash64 prime used as
      // multiplier, and a reversed-order mix. Independent of mix_lo.
      x ^= x >> 31; x *= 0x9fb21c651e98df25ULL;
      x ^= x >> 27; x *= 0x85ebca77c2b2ae63ULL;
      x ^= x >> 33;
      return x;
    };
    uint64_t l1_lo = 0, l1_hi = 0;

    for (auto v_it = super_comp().varsBegin(); *v_it != varsSENTINEL;  v_it++)
      if (var_seen(*v_it)) { //we have to put a var into our component
        p_new_comp->addVar(*v_it);
        current_comp_for_caching_.addVar(*v_it);
        setVar_in_other_comp(*v_it);
        uint64_t id = (uint64_t)*v_it;
        l1_lo ^= mix_lo(id);
        l1_hi ^= mix_hi(id);
      }
    p_new_comp->closeVariableData();
    current_comp_for_caching_.closeVariableData();

    for (auto it_cl = super_comp().clsBegin(); *it_cl != clsSENTINEL; it_cl++)
      if (clause_seen(*it_cl)) {
        p_new_comp->addCl(*it_cl);
           if(!clause_all_lits_active(*it_cl))
             current_comp_for_caching_.addCl(*it_cl);
        setClause_in_other_comp(*it_cl);
        // Namespace-separate clause IDs from var IDs so they can't
        // collide in either half.
        uint64_t cid = (uint64_t)*it_cl ^ 0x9e3779b97f4a7c15ULL;
        l1_lo ^= mix_lo(cid);
        // Use a different namespace constant for the hi half so
        // identical clause IDs contribute differently there too.
        uint64_t cid_hi = (uint64_t)*it_cl ^ 0xbf58476d1ce4e5b9ULL;
        l1_hi ^= mix_hi(cid_hi);
      }
    p_new_comp->closeClauseData();
    current_comp_for_caching_.closeClauseData();
    p_new_comp->setL1Hash(l1_lo, l1_hi);
    return p_new_comp;
  }
//  Component *makeComponentFromState(unsigned stack_size) {
//      Component *p_new_comp = new Component();
//      p_new_comp->reserveSpace(stack_size, super_comp().numLongClauses());
//
//      for (auto v_it = super_comp().varsBegin(); *v_it != varsSENTINEL;  v_it++)
//        if (var_seen(*v_it)) { //we have to put a var into our component
//          p_new_comp->addVar(*v_it);
//          setVar_in_other_comp(*v_it);
//        }
//      p_new_comp->closeVariableData();
//
//      for (auto it_cl = super_comp().clsBegin(); *it_cl != clsSENTINEL; it_cl++)
//        if (clause_seen(*it_cl)) {
//          p_new_comp->addCl(*it_cl);
//          setClause_in_other_comp(*it_cl);
//        }
//      p_new_comp->closeClauseData();
//      return p_new_comp;
//    }

  Component current_comp_for_caching_;
private:
  Component *p_super_comp_;
  StackLevel *p_stack_level_;

  static CA_SearchState *seen_;
  static unsigned seen_byte_size_;

};





// createComponents removed — using content-based cache

#endif /* COMPONENT_ARCHETYPE_H_ */
