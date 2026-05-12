/*
 * component_management.cpp
 *
 *  Created on: Aug 23, 2012
 *      Author: Marc Thurley
 *
 *  Modified: removed ID-based cache initialization
 */

#include "component_management.h"

void ComponentManager::initialize(LiteralIndexedVector<Literal> & literals,
    vector<LiteralID> &lit_pool,
    unsigned original_lit_pool_size) {

  ana_.initialize(literals, lit_pool, original_lit_pool_size);

  component_stack_.clear();
  component_stack_.reserve(ana_.max_variable_id() + 2);
  component_stack_.push_back(new Component());
  component_stack_.push_back(new Component());
  assert(component_stack_.size() == 2);
  component_stack_.back()->createAsDummyComponent(ana_.max_variable_id(),
      ana_.max_clause_id());
}
