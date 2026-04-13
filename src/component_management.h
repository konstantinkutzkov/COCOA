/*
 * component_management.h
 *
 *  Created on: Aug 23, 2012
 *      Author: Marc Thurley
 *
 *  Modified: content-based caching using canonical formula keys
 */

#ifndef COMPONENT_MANAGEMENT_H_
#define COMPONENT_MANAGEMENT_H_

#include "component_types/component.h"
#include "alt_component_analyzer.h"
#include "content_cache.h"

#include <vector>
#include <gmpxx.h>
#include "containers.h"
#include "stack.h"

#include "solver_config.h"
using namespace std;

typedef AltComponentAnalyzer ComponentAnalyzer;

class ComponentManager {
public:
  ComponentManager(SolverConfiguration &config, DataAndStatistics &statistics,
        LiteralIndexedVector<TriValue> & lit_values) :
        config_(config), statistics_(statistics),
        ana_(statistics,lit_values) {
  }

  void initialize(LiteralIndexedVector<Literal> & literals,
        vector<LiteralID> &lit_pool);

  unsigned scoreOf(VariableIndex v) {
      return ana_.scoreOf(v);
  }

  ClauseOfs clauseOfsOf(ClauseIndex id) {
      return ana_.clauseOfsOf(id);
  }

  void setRemovedClauses(const std::unordered_map<ClauseOfs, unsigned> *p) {
      ana_.setRemovedClauses(p);
      removed_clauses_ = p;
  }

  void setFormulaRefs(LiteralIndexedVector<Literal> *literals,
                      vector<LiteralID> *lit_pool,
                      unsigned original_lit_pool_size) {
      literals_ = literals;
      lit_pool_ = lit_pool;
      original_lit_pool_size_ = original_lit_pool_size;
  }

  void cacheModelCountOf(unsigned stack_comp_id, const mpz_class &value) {
    if (config_.perform_component_caching) {
      Component *comp = component_stack_[stack_comp_id];
      if (comp->hasCanonicalKey()) {
        content_cache_.store(*comp->canonical_key_, value);
      }
    }
  }

  ContentCache &contentCache() { return content_cache_; }

  Component & superComponentOf(StackLevel &lev) {
    assert(component_stack_.size() > lev.super_component());
    return *component_stack_[lev.super_component()];
  }

  unsigned component_stack_size() {
    return component_stack_.size();
  }

  void cleanRemainingComponentsOf(StackLevel &top) {
    while (component_stack_.size() > top.remaining_components_ofs()) {
      delete component_stack_.back();
      component_stack_.pop_back();
    }
    assert(top.remaining_components_ofs() <= component_stack_.size());
  }

  Component & currentRemainingComponentOf(StackLevel &top) {
    assert(component_stack_.size() > top.currentRemainingComponent());
    return *component_stack_[top.currentRemainingComponent()];
  }

  // checks for the next yet to explore remaining component of top
  // returns true if a non-trivial non-cached component
  // has been found and is now stack_.TOS_NextComp()
  // returns false if all components have been processed;
  inline bool findNextRemainingComponentOf(StackLevel &top);

  inline void recordRemainingCompsFor(StackLevel &top);

  inline void sortComponentStackRange(unsigned start, unsigned end);

  void gatherStatistics(){
    statistics_.num_cached_components_ = content_cache_.size();
    statistics_.num_cache_hits_ = content_cache_.stats_hits;
    statistics_.num_cache_look_ups_ = content_cache_.stats_hits + content_cache_.stats_misses;
  }

  // No-op: content-based cache doesn't have pollution
  void removeAllCachePollutionsOf(StackLevel &top) {
    (void)top;
  }

private:

  SolverConfiguration &config_;
  DataAndStatistics &statistics_;

  // Content-based cache
  ContentCache content_cache_;

  // References for building canonical keys
  LiteralIndexedVector<Literal> *literals_ = nullptr;
  vector<LiteralID> *lit_pool_ = nullptr;
  const std::unordered_map<ClauseOfs, unsigned> *removed_clauses_ = nullptr;
  unsigned original_lit_pool_size_ = 0;

  vector<Component *> component_stack_;
  ComponentAnalyzer ana_;
};


void ComponentManager::sortComponentStackRange(unsigned start, unsigned end){
    assert(start <= end);
    for (unsigned i = start; i < end; i++)
      for (unsigned j = i + 1; j < end; j++) {
        if (component_stack_[i]->num_variables()
            < component_stack_[j]->num_variables())
          swap(component_stack_[i], component_stack_[j]);
      }
  }

bool ComponentManager::findNextRemainingComponentOf(StackLevel &top) {
    if (component_stack_.size() <= top.remaining_components_ofs())
      recordRemainingCompsFor(top);
    if (top.branch_found_unsat())
      return false;
    if (top.hasUnprocessedComponents())
      return true;
    top.includeSolution(1);
    return false;
  }


void ComponentManager::recordRemainingCompsFor(StackLevel &top) {
   Component & super_comp = superComponentOf(top);
   unsigned new_comps_start_ofs = component_stack_.size();

   ana_.setupAnalysisContext(top, super_comp);

   for (auto vt = super_comp.varsBegin(); *vt != varsSENTINEL; vt++)
     if (ana_.isUnseenAndActive(*vt)) {
         if (ana_.exploreRemainingCompOf(*vt)){

           Component *p_new_comp = ana_.makeComponentFromArcheType();

           // Build canonical key and try content cache
           if (config_.perform_component_caching && literals_ && lit_pool_) {
             static const std::unordered_map<ClauseOfs, unsigned> empty_removed;
             const auto &rm = removed_clauses_ ? *removed_clauses_ : empty_removed;

             CanonicalKey key = buildCanonicalKey(
               *p_new_comp, *lit_pool_, *literals_, ana_.literalValues(),
               ana_.clauseIdToOfs(), rm, original_lit_pool_size_);

             mpz_class cached_count;
             if (content_cache_.lookup(key, cached_count)) {
               top.includeSolution(cached_count);
               delete p_new_comp;
               if (config_.verbose)
                 cout << "  CACHE HIT count=" << cached_count << endl;
               if (top.branch_found_unsat())
                 goto done_recording;  // 0-count hit → stop processing
               continue;
             }
             // Cache miss: store key in component for later cacheModelCountOf
             p_new_comp->setCanonicalKey(std::make_shared<CanonicalKey>(std::move(key)));
             component_stack_.push_back(p_new_comp);
           } else {
             // Caching disabled: just store component
             component_stack_.push_back(p_new_comp);
           }
         }
     }

done_recording:
   top.set_unprocessed_components_end(component_stack_.size());
   sortComponentStackRange(new_comps_start_ofs, component_stack_.size());
}

#endif /* COMPONENT_MANAGEMENT_H_ */
