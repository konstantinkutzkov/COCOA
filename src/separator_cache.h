/*
 * separator_cache.h
 *
 * Separator cache with KMV sketch-based similarity lookup, transfer,
 * greedy repair, and negative caching.
 */

#ifndef SEPARATOR_CACHE_H_
#define SEPARATOR_CACHE_H_

#include <vector>
#include <set>
#include <unordered_set>
#include <algorithm>
#include <cstdint>
#include <cassert>
#include <iostream>

#include "separator.h"

// -----------------------------------------------------------------------
// KMV Sketch
// -----------------------------------------------------------------------

class KMVSketch {
public:
  int k_;
  int max_stored_;
  std::vector<uint64_t> values_;  // sorted ascending, smallest first

  KMVSketch(int k = 128, int reservoir_factor = 4)
    : k_(k), max_stored_(k * reservoir_factor) {}

  static uint64_t mix64(uint64_t x) {
    x = ((x >> 30) ^ x) * 0xBF58476D1CE4E5B9ULL;
    x = ((x >> 27) ^ x) * 0x94D049BB133111EBULL;
    x = (x >> 31) ^ x;
    return x;
  }

  void build(const std::vector<unsigned> &items) {
    // Hash all items, deduplicate, sort, keep smallest max_stored_
    std::set<uint64_t> hset;
    for (unsigned item : items)
      hset.insert(mix64(item));
    values_.clear();
    values_.reserve(std::min((int)hset.size(), max_stored_));
    int count = 0;
    for (uint64_t h : hset) {
      if (count >= max_stored_) break;
      values_.push_back(h);
      count++;
    }
  }

  float estimateJaccard(const KMVSketch &other) const {
    if (values_.empty() || other.values_.empty())
      return 0.0f;

    int k = std::min({k_, (int)values_.size(), (int)other.values_.size()});
    if (k == 0) return 0.0f;

    const auto &a = values_;
    const auto &b = other.values_;
    int i = 0, j = 0;
    int union_count = 0, intersection_count = 0;

    while (union_count < k && i < (int)a.size() && j < (int)b.size()) {
      if (a[i] == b[j]) {
        intersection_count++;
        union_count++;
        i++; j++;
      } else if (a[i] < b[j]) {
        union_count++;
        i++;
      } else {
        union_count++;
        j++;
      }
    }
    while (union_count < k && i < (int)a.size()) { union_count++; i++; }
    while (union_count < k && j < (int)b.size()) { union_count++; j++; }

    if (union_count == 0) return 0.0f;
    return (float)intersection_count / (float)union_count;
  }
};

// -----------------------------------------------------------------------
// Cached Entry
// -----------------------------------------------------------------------

struct CachedSeparatorEntry {
  std::vector<CutNode> separator;
  std::set<unsigned> partition_a;   // variable IDs on side A
  std::set<unsigned> partition_b;   // variable IDs on other sides
  KMVSketch sketch;
  std::vector<int> component_sizes;
  int reuse_count = 0;
};

// -----------------------------------------------------------------------
// Separator Cache
// -----------------------------------------------------------------------

class SeparatorCache {
public:
  int max_entries = 500;
  int sketch_k = 128;
  int sketch_reservoir_factor = 4;
  float j_min = 0.3f;
  float j_merge = 0.95f;
  int repair_budget = 3;
  int min_second_component_vars = 5;
  int max_separator_size = 12;

  // Negative cache
  float negative_j_threshold = 0.85f;
  float negative_j_merge = 0.95f;
  int negative_max = 500;

  // Stats
  int stats_transfer_ok = 0;
  int stats_repair_ok = 0;
  int stats_miss = 0;
  int stats_negative_hit = 0;

  bool verbose = false;

  std::vector<CachedSeparatorEntry> entries;
  std::vector<KMVSketch> negative_sketches;

  // ------------------------------------------------------------------
  // Sketch helpers
  // ------------------------------------------------------------------

  KMVSketch computeSketch(const FormulaInfo &info) {
    KMVSketch sk(sketch_k, sketch_reservoir_factor);
    sk.build(info.active_clause_ids);
    return sk;
  }

  // ------------------------------------------------------------------
  // Insert
  // ------------------------------------------------------------------

  void insert(const FormulaInfo &info,
              const std::vector<CutNode> &separator,
              const std::set<unsigned> &partition_a,
              const std::set<unsigned> &partition_b,
              const std::vector<int> &component_sizes,
              const KMVSketch &sketch) {
    // Diversity gate
    for (const auto &e : entries) {
      if (sketch.estimateJaccard(e.sketch) >= j_merge)
        return;
    }

    if ((int)entries.size() >= max_entries)
      evict();

    CachedSeparatorEntry entry;
    entry.separator = separator;
    entry.partition_a = partition_a;
    entry.partition_b = partition_b;
    entry.sketch = sketch;
    entry.component_sizes = component_sizes;
    entry.reuse_count = 0;
    entries.push_back(std::move(entry));
  }

  void insertNegative(const KMVSketch &sketch) {
    for (const auto &ns : negative_sketches) {
      if (sketch.estimateJaccard(ns) >= negative_j_merge)
        return;
    }
    if ((int)negative_sketches.size() >= negative_max)
      negative_sketches.erase(negative_sketches.begin());
    negative_sketches.push_back(sketch);
  }

  bool isLikelyNegative(const KMVSketch &sketch) {
    for (const auto &ns : negative_sketches) {
      if (sketch.estimateJaccard(ns) >= negative_j_threshold) {
        stats_negative_hit++;
        return true;
      }
    }
    return false;
  }

  // ------------------------------------------------------------------
  // Query: find and transfer
  // ------------------------------------------------------------------

  // Returns true if a separator was found; fills result_sep and result_sizes.
  bool findAndTransfer(const FormulaInfo &info,
                       const KMVSketch &current_sketch,
                       std::vector<CutNode> &result_sep,
                       std::vector<int> &result_sizes) {
    if (entries.empty()) return false;

    // Rank candidates by Jaccard
    struct Candidate {
      float jaccard;
      int index;
    };
    std::vector<Candidate> candidates;
    for (int i = 0; i < (int)entries.size(); i++) {
      float j = current_sketch.estimateJaccard(entries[i].sketch);
      if (j >= j_min)
        candidates.push_back({j, i});
    }
    if (candidates.empty()) {
      stats_miss++;
      return false;
    }
    std::sort(candidates.begin(), candidates.end(),
      [](const Candidate &a, const Candidate &b) { return a.jaccard > b.jaccard; });

    // Build sets of active vars and clauses for quick lookup
    std::unordered_set<unsigned> active_vars(info.active_vars.begin(), info.active_vars.end());
    std::unordered_set<unsigned> active_clauses(info.active_clause_ids.begin(), info.active_clause_ids.end());

    for (const auto &cand : candidates) {
      auto &entry = entries[cand.index];
      const auto &sep = entry.separator;

      // Pre-filter: all separator elements must still be active
      if (!sepElementsActive(sep, active_vars, active_clauses))
        continue;

      // Phase 1: direct transfer (BFS verification)
      auto sizes = verify_separator(info, sep, min_second_component_vars);
      if (!sizes.empty()) {
        entry.reuse_count++;
        stats_transfer_ok++;
        result_sep = sep;
        result_sizes = sizes;
        if (verbose)
          std::cout << "[cache] transfer |S|=" << sep.size()
                    << " sim=" << cand.jaccard << std::endl;
        return true;
      }

      // Phase 2: greedy repair
      auto repaired = greedyRepair(info, sep,
        entry.partition_a, entry.partition_b,
        active_vars, active_clauses);
      if (!repaired.empty()) {
        entry.reuse_count++;
        stats_repair_ok++;
        auto rep_sizes = verify_separator(info, repaired, min_second_component_vars);
        if (!rep_sizes.empty()) {
          result_sep = repaired;
          result_sizes = rep_sizes;
          if (verbose)
            std::cout << "[cache] repaired |S|=" << repaired.size()
                      << " (was " << sep.size() << ") sim=" << cand.jaccard << std::endl;
          return true;
        }
      }
    }

    stats_miss++;
    return false;
  }

private:

  void evict() {
    if (entries.empty()) return;
    int min_idx = 0;
    int min_reuse = entries[0].reuse_count;
    for (int i = 1; i < (int)entries.size(); i++) {
      if (entries[i].reuse_count < min_reuse) {
        min_reuse = entries[i].reuse_count;
        min_idx = i;
      }
    }
    entries.erase(entries.begin() + min_idx);
  }

  static bool sepElementsActive(const std::vector<CutNode> &sep,
                                 const std::unordered_set<unsigned> &active_vars,
                                 const std::unordered_set<unsigned> &active_clauses) {
    for (const auto &nd : sep) {
      if (nd.kind == CutNode::VAR && active_vars.find(nd.id) == active_vars.end())
        return false;
      if (nd.kind == CutNode::CLAUSE && active_clauses.find(nd.id) == active_clauses.end())
        return false;
    }
    return true;
  }

  // ------------------------------------------------------------------
  // Greedy repair
  // ------------------------------------------------------------------

  std::vector<CutNode> greedyRepair(
      const FormulaInfo &info,
      const std::vector<CutNode> &separator,
      const std::set<unsigned> &partition_a,
      const std::set<unsigned> &partition_b,
      const std::unordered_set<unsigned> &active_vars,
      const std::unordered_set<unsigned> &active_clauses) {

    std::set<CutNode> current_sep(separator.begin(), separator.end());
    std::unordered_set<unsigned> sep_var_ids;
    for (const auto &nd : current_sep)
      if (nd.kind == CutNode::VAR)
        sep_var_ids.insert(nd.id);

    for (int round = 0; round < repair_budget; round++) {
      if ((int)current_sep.size() >= max_separator_size)
        break;

      // Find crossing clauses
      auto crossing = findCrossingClauses(info, current_sep, sep_var_ids,
                                           partition_a, partition_b, active_vars);
      if (crossing.empty())
        break;

      // Greedy: pick element covering most crossings
      CutNode best_node;
      int best_score = 0;
      bool found_best = false;

      // Score variables
      std::unordered_map<unsigned, int> var_cover;
      for (int ci : crossing) {
        for (unsigned var_id : info.clause_variables[ci]) {
          if (sep_var_ids.count(var_id) || !active_vars.count(var_id))
            continue;
          var_cover[var_id]++;
        }
      }
      for (const auto &vc : var_cover) {
        if (vc.second > best_score) {
          best_score = vc.second;
          best_node = CutNode(CutNode::VAR, vc.first);
          found_best = true;
        }
      }

      // Score clauses
      for (int ci : crossing) {
        unsigned cid = info.active_clause_ids[ci];
        if (current_sep.count(CutNode(CutNode::CLAUSE, cid)))
          continue;
        int score = 1;
        if (score > best_score) {
          best_score = score;
          best_node = CutNode(CutNode::CLAUSE, cid);
          found_best = true;
        }
      }

      if (!found_best) break;

      current_sep.insert(best_node);
      if (best_node.kind == CutNode::VAR)
        sep_var_ids.insert(best_node.id);
    }

    // Verify
    std::vector<CutNode> sep_list(current_sep.begin(), current_sep.end());
    auto sizes = verify_separator(info, sep_list, min_second_component_vars);
    if (sizes.empty())
      return {};

    // Refine: try dropping each element
    sep_list = refine(info, sep_list);
    sizes = verify_separator(info, sep_list, min_second_component_vars);
    if (sizes.empty())
      return {};

    return sep_list;
  }

  std::vector<int> findCrossingClauses(
      const FormulaInfo &info,
      const std::set<CutNode> &current_sep,
      const std::unordered_set<unsigned> &sep_var_ids,
      const std::set<unsigned> &partition_a,
      const std::set<unsigned> &partition_b,
      const std::unordered_set<unsigned> &active_vars) {

    std::vector<int> crossing;
    for (int ci = 0; ci < (int)info.active_clause_ids.size(); ci++) {
      unsigned cid = info.active_clause_ids[ci];
      if (current_sep.count(CutNode(CutNode::CLAUSE, cid)))
        continue;

      bool has_a = false, has_b = false;
      for (unsigned var_id : info.clause_variables[ci]) {
        if (sep_var_ids.count(var_id) || !active_vars.count(var_id))
          continue;
        if (partition_a.count(var_id)) has_a = true;
        if (partition_b.count(var_id)) has_b = true;
        if (has_a && has_b) break;
      }
      if (has_a && has_b)
        crossing.push_back(ci);
    }
    return crossing;
  }

  std::vector<CutNode> refine(const FormulaInfo &info,
                               const std::vector<CutNode> &separator) {
    std::vector<CutNode> refined = separator;
    for (const auto &nd : separator) {
      std::vector<CutNode> candidate;
      for (const auto &n : refined)
        if (!(n == nd))
          candidate.push_back(n);
      if (candidate.empty()) continue;
      auto sizes = verify_separator(info, candidate, min_second_component_vars);
      if (!sizes.empty())
        refined = candidate;
    }
    return refined;
  }
};

#endif /* SEPARATOR_CACHE_H_ */
