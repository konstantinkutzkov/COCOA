/*
 * instance.cpp
 *
 *  Created on: Aug 23, 2012
 *      Author: Marc Thurley
 */

#include "instance.h"

#include <algorithm>
#include <climits>
#include <fstream>
#include <random>
#include <sys/stat.h>
#include <unordered_map>

using namespace std;

// Derivative-cache content-aware hash init (Phase 3, SUM scheme).
// Generates lit_hashes_, allocates per-clause arrays sized for original
// clauses only, walks every original long clause and computes initial
// content_hash = clause_random[C] + SUM of lit_hashes for X_TRI lits.
// After this returns, deriv_cache_hooks_enabled_ is true and
// setLiteralIfFree / unSet will keep the invariants. See instance.h.
void Instance::deriv_cache_track_init_() {
  // Per-literal random hashes, indexed by LiteralID::raw().
  // raw() = (var << 1) + sign; max value is (max_var << 1) + 1.
  // variables_.size() is num_variables+1 (1-indexed, with slot 0 unused).
  const size_t n_lit_slots = variables_.size() * 2;
  deriv_cache_lit_hashes_.assign(n_lit_slots, 0);
  std::mt19937_64 lit_rng(0x9e3779b97f4a7c15ULL);
  for (size_t i = 0; i < n_lit_slots; ++i) {
    deriv_cache_lit_hashes_[i] = lit_rng();
  }

  // Per-clause arrays: sized to original_lit_pool_size_ so we can index
  // any original clause's offset without bounds checks. Learned clauses
  // (ofs >= original_lit_pool_size_) are NOT tracked here — Solver falls
  // back to identity hashing for them.
  deriv_cache_clause_content_hash_.assign(original_lit_pool_size_, 0);
  deriv_cache_clause_sat_count_.assign(original_lit_pool_size_, 0);

  // Per-clause clause_random baseline: a uint64 per clause, generated
  // here and folded into the initial content_hash. Stays constant for
  // the life of the clause — the hooks only add/sub lit_hashes. Uses a
  // separate seed so the clause_random sequence is uncorrelated with
  // lit_hashes_.
  std::mt19937_64 cr_rng(0xa3b195354a39b70dULL);

  // Walk every original long clause. Iteration mirrors
  // Solver::deriv_cache_init_'s pool walk: header overhead + clause
  // body terminated by SENTINEL_LIT. The aggregation INSIDE the clause
  // is SUM (mod 2^64), not XOR — see instance.h comment for why.
  ClauseOfs cl_ofs = (ClauseOfs)(1 + ClauseHeader::overheadInLits());
  while (cl_ofs < (ClauseOfs)original_lit_pool_size_) {
    uint64_t h = cr_rng();           // per-clause clause_random baseline
    uint16_t sat_count = 0;
    auto it = literal_pool_.begin() + cl_ofs;
    for (; *it != SENTINEL_LIT; ++it) {
      const TriValue v = literal_values_[*it];
      if (v == X_TRI) {
        h += deriv_cache_lit_hashes_[it->raw()];
      } else if (v == T_TRI) {
        sat_count++;
      }
    }
    deriv_cache_clause_content_hash_[cl_ofs] = h;
    deriv_cache_clause_sat_count_[cl_ofs] = sat_count;
    cl_ofs = (ClauseOfs)((it - literal_pool_.begin()) + 1
                          + ClauseHeader::overheadInLits());
  }

  // ---------------------------------------------------------------
  // Per-original-BINARY-clause init. Binary clauses live in
  // Literal::binary_links_ (no ClauseOfs), so they need their own
  // id-space + per-clause storage. Each (lit_a, lit_b) original binary
  // gets one id; both endpoints store that id in their
  // binary_link_ids_ array at the same index as the partner in
  // binary_links_. Convention: lit_a.raw() < lit_b.raw() is the
  // canonical direction at id-assignment time.
  // ---------------------------------------------------------------
  {
    std::mt19937_64 br_rng(0x7c8f5b9c3a1d4e6fULL);
    // First pass: count + assign ids. Use a temp map for lookups from
    // the other endpoint; freed at end-of-scope so it doesn't persist.
    std::unordered_map<uint64_t, unsigned> pair_to_id;
    unsigned next_id = 0;
    for (unsigned raw = 0; raw < literals_.size(); ++raw) {
      LiteralID A((raw >> 1), (raw & 1));
      Literal &la = literals_[A];
      const unsigned orig = la.original_binary_link_count_;
      for (unsigned i = 0; i < orig; ++i) {
        LiteralID B = la.binary_links_[i];
        if (A.raw() >= B.raw()) continue;  // canonical direction only
        const uint64_t key = ((uint64_t)A.raw() << 32) | (uint64_t)B.raw();
        pair_to_id[key] = next_id++;
      }
    }
    deriv_cache_binary_clause_content_hash_.assign(next_id, 0);
    deriv_cache_binary_clause_sat_count_.assign(next_id, 0);

    // Second pass: populate Literal::binary_link_ids_ for every original
    // entry from both endpoints, AND initialize the per-binary
    // content_hash + sat_count from the canonical (lit_a < lit_b) side.
    for (unsigned raw = 0; raw < literals_.size(); ++raw) {
      LiteralID A((raw >> 1), (raw & 1));
      Literal &la = literals_[A];
      const unsigned orig = la.original_binary_link_count_;
      la.binary_link_ids_.assign(orig, UINT_MAX);
      for (unsigned i = 0; i < orig; ++i) {
        LiteralID B = la.binary_links_[i];
        const uint64_t key = A.raw() < B.raw()
            ? ((uint64_t)A.raw() << 32) | (uint64_t)B.raw()
            : ((uint64_t)B.raw() << 32) | (uint64_t)A.raw();
        auto it = pair_to_id.find(key);
        if (it != pair_to_id.end()) {
          la.binary_link_ids_[i] = it->second;
        }
      }
    }
    // Initialize content_hash + sat_count by walking from the canonical
    // (A.raw() < B.raw()) direction only — visits each binary exactly once.
    for (unsigned raw = 0; raw < literals_.size(); ++raw) {
      LiteralID A((raw >> 1), (raw & 1));
      Literal &la = literals_[A];
      const unsigned orig = la.original_binary_link_count_;
      for (unsigned i = 0; i < orig; ++i) {
        LiteralID B = la.binary_links_[i];
        if (A.raw() >= B.raw()) continue;
        const unsigned id = la.binary_link_ids_[i];
        uint64_t h = br_rng();           // per-binary clause_random baseline
        uint16_t sat_count = 0;
        const TriValue va = literal_values_[A];
        const TriValue vb = literal_values_[B];
        if (va == X_TRI) h += deriv_cache_lit_hashes_[A.raw()];
        else if (va == T_TRI) sat_count++;
        if (vb == X_TRI) h += deriv_cache_lit_hashes_[B.raw()];
        else if (vb == T_TRI) sat_count++;
        deriv_cache_binary_clause_content_hash_[id] = h;
        deriv_cache_binary_clause_sat_count_[id] = sat_count;
      }
    }
  }

  // Hooks now safe to fire. Any subsequent setLiteralIfFree / unSet
  // maintains the invariants.
  deriv_cache_hooks_enabled_ = true;
}

void Instance::cleanClause(ClauseOfs cl_ofs) {
  bool satisfied = false;
  for (auto it = beginOf(cl_ofs); *it != SENTINEL_LIT; it++)
    if (isSatisfied(*it)) {
      satisfied = true;
      break;
    }
  // mark the clause as empty if satisfied
  if (satisfied) {
    *beginOf(cl_ofs) = SENTINEL_LIT;
    return;
  }
  auto jt = beginOf(cl_ofs);
  auto it = beginOf(cl_ofs);
  // from now, all inactive literals are resolved
  for (; *it != SENTINEL_LIT; it++, jt++) {
    while (*jt != SENTINEL_LIT && !isActive(*jt))
      jt++;
    *it = *jt;
    if (*jt == SENTINEL_LIT)
      break;
  }
  unsigned length = it - beginOf(cl_ofs);
  // if it has become a unit clause, it should have already been asserted
  if (length == 1) {
    *beginOf(cl_ofs) = SENTINEL_LIT;
    // if it has become binary, transform it to binary and delete it
  } else if (length == 2) {
    addBinaryClause(*beginOf(cl_ofs), *(beginOf(cl_ofs) + 1));
    *beginOf(cl_ofs) = SENTINEL_LIT;
  }
}

void Instance::compactClauses() {
  vector<ClauseOfs> clause_ofs;
  clause_ofs.reserve(statistics_.num_long_clauses_);

  // clear watch links and occurrence lists
  for (auto it_lit = literal_pool_.begin(); it_lit != literal_pool_.end();
      it_lit++) {
    if (*it_lit == SENTINEL_LIT) {
      if (it_lit + 1 == literal_pool_.end())
        break;
      it_lit += ClauseHeader::overheadInLits();
      clause_ofs.push_back(1 + it_lit - literal_pool_.begin());
    }
  }

  for (auto ofs : clause_ofs)
    cleanClause(ofs);

  for (auto &l : literals_)
    l.resetWatchList();

  occurrence_lists_.clear();
  occurrence_lists_.resize(variables_.size());

  vector<LiteralID> tmp_pool = literal_pool_;
  literal_pool_.clear();
  literal_pool_.push_back(SENTINEL_LIT);
  ClauseOfs new_ofs;
  unsigned num_clauses = 0;
  for (auto ofs : clause_ofs) {
    auto it = (tmp_pool.begin() + ofs);
    if (*it != SENTINEL_LIT) {
      for (unsigned i = 0; i < ClauseHeader::overheadInLits(); i++)
        literal_pool_.push_back(0);
      new_ofs = literal_pool_.size();
      literal(*it).addWatchLinkTo(new_ofs);
      literal(*(it + 1)).addWatchLinkTo(new_ofs);
      num_clauses++;
      for (; *it != SENTINEL_LIT; it++) {
        literal_pool_.push_back(*it);
        occurrence_lists_[*it].push_back(new_ofs);
      }
      literal_pool_.push_back(SENTINEL_LIT);
    }
  }

  vector<LiteralID> tmp_bin;
  unsigned bin_links = 0;
  for (auto &l : literals_) {
    tmp_bin.clear();
    for (auto it = l.binary_links_.begin(); *it != SENTINEL_LIT; it++)
      if (isActive(*it))
        tmp_bin.push_back(*it);
    bin_links += tmp_bin.size();
    tmp_bin.push_back(SENTINEL_LIT);
    l.binary_links_ = tmp_bin;
  }
  statistics_.num_long_clauses_ = num_clauses;
  statistics_.num_binary_clauses_ = bin_links >> 1;
}

void Instance::compactVariables() {
  vector<unsigned> var_map(variables_.size(), 0);
  unsigned last_ofs = 0;
  unsigned num_isolated = 0;
  LiteralIndexedVector<vector<LiteralID> > _tmp_bin_links(1);
  LiteralIndexedVector<TriValue> _tmp_values = literal_values_;

  for (auto l : literals_)
    _tmp_bin_links.push_back(l.binary_links_);

  assert(_tmp_bin_links.size() == literals_.size());
  // If a previous compactVariables already ran, compact_to_orig_[v] tells
  // us the *original* variable number for the pre-remap v. Otherwise v is
  // already original.
  std::vector<unsigned> prev_to_orig = compact_to_orig_;
  auto to_orig = [&](unsigned v) {
    return (v < prev_to_orig.size() && !prev_to_orig.empty()) ? prev_to_orig[v] : v;
  };
  std::vector<unsigned> new_to_orig(1, 0);  // index 0 unused
  for (unsigned v = 1; v < variables_.size(); v++)
    if (isActive(v)) {
      if (isolated(v)) {
        num_isolated++;
        continue;
      }
      last_ofs++;
      var_map[v] = last_ofs;
      new_to_orig.push_back(to_orig(v));
    }

  variables_.clear();
  variables_.resize(last_ofs + 1);
  is_branch_constraint_.assign(variables_.size(), false);
  occurrence_lists_.clear();
  occurrence_lists_.resize(variables_.size());
  literals_.clear();
  literals_.resize(variables_.size());
  literal_values_.clear();
  literal_values_.resize(variables_.size(), X_TRI);

  unsigned bin_links = 0;
  LiteralID newlit;
  for (auto l = LiteralID(0, false); l != _tmp_bin_links.end_lit(); l.inc()) {
    if (var_map[l.var()] != 0) {
      newlit = LiteralID(var_map[l.var()], l.sign());
      for (auto it = _tmp_bin_links[l].begin(); *it != SENTINEL_LIT; it++) {
        assert(var_map[it->var()] != 0);
        literals_[newlit].addBinLinkTo(
            LiteralID(var_map[it->var()], it->sign()));
      }
      bin_links += literals_[newlit].binary_links_.size() - 1;
    }
  }

  vector<ClauseOfs> clause_ofs;
  clause_ofs.reserve(statistics_.num_long_clauses_);
  // clear watch links and occurrence lists
  for (auto it_lit = literal_pool_.begin(); it_lit != literal_pool_.end();
      it_lit++) {
    if (*it_lit == SENTINEL_LIT) {
      if (it_lit + 1 == literal_pool_.end())
        break;
      it_lit += ClauseHeader::overheadInLits();
      clause_ofs.push_back(1 + it_lit - literal_pool_.begin());
    }
  }

  for (auto ofs : clause_ofs) {
    literal(LiteralID(var_map[beginOf(ofs)->var()], beginOf(ofs)->sign())).addWatchLinkTo(
        ofs);
    literal(LiteralID(var_map[(beginOf(ofs) + 1)->var()],
            (beginOf(ofs) + 1)->sign())).addWatchLinkTo(ofs);
    for (auto it_lit = beginOf(ofs); *it_lit != SENTINEL_LIT; it_lit++) {
      *it_lit = LiteralID(var_map[it_lit->var()], it_lit->sign());
      occurrence_lists_[*it_lit].push_back(ofs);
    }
  }

  literal_values_.clear();
  literal_values_.resize(variables_.size(), X_TRI);
  unit_clauses_.clear();

  statistics_.num_variables_ = variables_.size() - 1 + num_isolated;

  statistics_.num_used_variables_ = num_variables();
  statistics_.num_free_variables_ = num_isolated;

  compact_to_orig_ = std::move(new_to_orig);
}

void Instance::compactConflictLiteralPool(){
  auto write_pos = conflict_clauses_begin();
  vector<ClauseOfs> tmp_conflict_clauses = conflict_clauses_;
  conflict_clauses_.clear();
  for(auto clause_ofs: tmp_conflict_clauses){
    auto read_pos = beginOf(clause_ofs) - ClauseHeader::overheadInLits();
    for(unsigned i = 0; i < ClauseHeader::overheadInLits(); i++)
      *(write_pos++) = *(read_pos++);
    ClauseOfs new_ofs =  write_pos - literal_pool_.begin();
    conflict_clauses_.push_back(new_ofs);
    // first substitute antecedent if clause_ofs implied something
    if(isAntecedentOf(clause_ofs, *beginOf(clause_ofs)))
      var(*beginOf(clause_ofs)).ante = Antecedent(new_ofs);

    // now redo the watches
    literal(*beginOf(clause_ofs)).replaceWatchLinkTo(clause_ofs,new_ofs);
    literal(*(beginOf(clause_ofs)+1)).replaceWatchLinkTo(clause_ofs,new_ofs);
    // next, copy clause data
    assert(read_pos == beginOf(clause_ofs));
    while(*read_pos != SENTINEL_LIT)
      *(write_pos++) = *(read_pos++);
    *(write_pos++) = SENTINEL_LIT;
  }
  literal_pool_.erase(write_pos,literal_pool_.end());
}


bool Instance::deleteConflictClauses() {
  statistics_.times_conflict_clauses_cleaned_++;
  vector<ClauseOfs> tmp_conflict_clauses = conflict_clauses_;
  conflict_clauses_.clear();
  vector<double> tmp_ratios;
  double score, lifetime;
  for(auto clause_ofs: tmp_conflict_clauses){
    score = getHeaderOf(clause_ofs).score();
    lifetime = statistics_.num_conflicts_ - getHeaderOf(clause_ofs).creation_time();
   // tmp_ratios.push_back(score/lifetime);
    tmp_ratios.push_back(score);

  }
  vector<double> tmp_ratiosB = tmp_ratios;

  sort(tmp_ratiosB.begin(), tmp_ratiosB.end());

  double cutoff = tmp_ratiosB[tmp_ratiosB.size()/2];

  for(unsigned i = 0; i < tmp_conflict_clauses.size(); i++){
    if(tmp_ratios[i] < cutoff){
      if(!markClauseDeleted(tmp_conflict_clauses[i]))
        conflict_clauses_.push_back(tmp_conflict_clauses[i]);
    } else
      conflict_clauses_.push_back(tmp_conflict_clauses[i]);
  }
  return true;
}


void Instance::deleteAllConflictClauses() {
  vector<ClauseOfs> remaining;
  for (auto clause_ofs : conflict_clauses_) {
    if (!markClauseDeleted(clause_ofs))
      remaining.push_back(clause_ofs);  // antecedent — can't delete yet
  }
  conflict_clauses_ = remaining;
}

bool Instance::markClauseDeleted(ClauseOfs cl_ofs){
  // only first literal may possibly have cl_ofs as antecedent
  if(isAntecedentOf(cl_ofs, *beginOf(cl_ofs)))
    return false;

  literal(*beginOf(cl_ofs)).removeWatchLinkTo(cl_ofs);
  literal(*(beginOf(cl_ofs)+1)).removeWatchLinkTo(cl_ofs);
  return true;
}


bool Instance::createfromFile(const string &file_name) {
  unsigned int nVars, nCls;
  int lit;
  unsigned max_ignore = 1000000;
  unsigned clauses_added = 0;
  LiteralID llit;
  vector<LiteralID> literals;
  string idstring;
  char c;

  // clear everything
  literal_pool_.clear();
  literal_pool_.push_back(SENTINEL_LIT);

  variables_.clear();
  variables_.push_back(Variable()); //initializing the Sentinel
  literal_values_.clear();
  unit_clauses_.clear();

  ///BEGIN File input
  ifstream input_file(file_name);
  if (!input_file) {
    cerr << "Cannot open file: " << file_name << endl;
    exit(0);
  }

  struct stat filestatus;
  stat(file_name.c_str(), &filestatus);

  literals.reserve(10000);
  while (input_file >> c && c != 'p')
    input_file.ignore(max_ignore, '\n');
  if (!(input_file >> idstring && idstring == "cnf" && input_file >> nVars
      && input_file >> nCls)) {
    cerr << "Invalid CNF file" << endl;
    exit(0);
  }

  variables_.resize(nVars + 1);
  is_branch_constraint_.assign(nVars + 1, false);
  literal_values_.resize(nVars + 1, X_TRI);
  literal_pool_.reserve(filestatus.st_size);
  conflict_clauses_.reserve(2*nCls);
  occurrence_lists_.clear();
  occurrence_lists_.resize(nVars + 1);

  literals_.clear();
  literals_.resize(nVars + 1);

  while ((input_file >> c) && clauses_added < nCls) {
    input_file.unget(); //extracted a nonspace character to determine if we have a clause, so put it back
    if ((c == '-') || isdigit(c)) {
      literals.clear();
      bool skip_clause = false;
      while ((input_file >> lit) && lit != 0) {
        bool duplicate_literal = false;
        for (auto i : literals) {
          if (i.toInt() == lit) {
            duplicate_literal = true;
            break;
          }
          if (i.toInt() == -lit) {
            skip_clause = true;
            break;
          }
        }
        if (!duplicate_literal) {
          literals.push_back(lit);
        }
      }
      if (!skip_clause) {
        if (literals.empty()) {
          // Empty clause = trivially UNSAT formula. Don't add it to
          // any clause structure (size==0 falls into the size>=3 path
          // in addClause which has UB on empty input). Set the flag
          // and continue parsing so the rest of the file's syntax
          // is consumed normally; simplePreProcess will short-circuit.
          parsed_trivially_unsat_ = true;
          clauses_added++;  // count it so the nCls header check is consistent
        } else {
          clauses_added++;
          statistics_.incorporateClauseData(literals);
          ClauseOfs cl_ofs = addClause(literals);
          if (literals.size() >= 3)
            for (auto l : literals)
              occurrence_lists_[l].push_back(cl_ofs);
        }
      }
    }
    input_file.ignore(max_ignore, '\n');
  }
  ///END NEW
  input_file.close();
  //  /// END FILE input

  statistics_.num_variables_ = statistics_.num_original_variables_ = nVars;
  statistics_.num_used_variables_ = num_variables();
  statistics_.num_free_variables_ = nVars - num_variables();

  statistics_.num_original_clauses_ = nCls;

  statistics_.num_original_binary_clauses_ = statistics_.num_binary_clauses_;
  statistics_.num_original_unit_clauses_ = statistics_.num_unit_clauses_ =
      unit_clauses_.size();

  original_lit_pool_size_ = literal_pool_.size();
  return true;
}

