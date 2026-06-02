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
#include <set>
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
        vector<LiteralID> &lit_pool,
        unsigned original_lit_pool_size);

  unsigned scoreOf(VariableIndex v) {
      return ana_.scoreOf(v);
  }

  ClauseOfs clauseOfsOf(ClauseIndex id) {
      return ana_.clauseOfsOf(id);
  }

  ComponentAnalyzer &getAnalyzer() { return ana_; }

  void setRemovedClauses(const std::unordered_map<ClauseOfs, unsigned> *p) {
      ana_.setRemovedClauses(p);
  }

  ContentCache &contentCache() { return content_cache_; }

  Component & superComponentOf(StackLevel &lev) {
    assert(component_stack_.size() > lev.super_component());
    return *component_stack_[lev.super_component()];
  }

  unsigned component_stack_size() {
    return component_stack_.size();
  }

  // Accessor for OPEN_WORK snapshot — the caller iterates open
  // components on the stack at timeout to compute the worst-case
  // remaining-work bound.
  Component *componentAt(unsigned i) const {
    return (i < component_stack_.size()) ? component_stack_[i] : nullptr;
  }

  void gatherStatistics(){
    statistics_.num_cached_components_ = content_cache_.size();
    statistics_.num_cache_hits_ = content_cache_.stats_hits;
    statistics_.num_cache_look_ups_ = content_cache_.stats_hits + content_cache_.stats_misses;
  }

private:

  SolverConfiguration &config_;
  DataAndStatistics &statistics_;

  // Content-based cache
  ContentCache content_cache_;

  vector<Component *> component_stack_;
  ComponentAnalyzer ana_;
};

#endif /* COMPONENT_MANAGEMENT_H_ */
