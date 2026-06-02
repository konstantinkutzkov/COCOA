/******************************************
Copyright (C) 2026 Authors of GANAK fork (canonical-key port)
                   Original code Copyright (C) 2023 GANAK Authors,
                   ported from sharpsat-separator/src/canonical_key.h

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
***********************************************/

/*
 * canonical_key.hpp
 *
 * Fast canonical key for component caching using tabulation hashing.
 *
 * A global dictionary maps canonical clause types to random hash values.
 * Variable signatures are computed via one-pass accumulation of
 * dictionary values. No heap allocation per component.
 *
 * Ganak port of sharpsat-separator/src/canonical_key.{h,cpp} (Commit 3 of
 * the canonical-key port, see sharpsat-separator/docs/
 * ganak_canonical_key_port_plan.md). Type translation per §4; iterator-shape
 * rewrites (SENTINEL_LIT-bounded literal-pool walks -> Clause::begin()/end())
 * per §4.1. removed_clauses + clause_id_to_ofs dropped entirely; the
 * solver's clause-branching machinery has no ganak analogue.
 *
 * As of Commit 3 this code COMPILES AND LINKS but is not yet called from
 * anywhere — the CANONICAL dispatch case in comp_management.hpp still
 * exits with the Commit-2 placeholder. Commit 4 wires it up.
 */

#pragma once

#include <vector>
#include <algorithm>
#include <unordered_map>
#include <cstdint>
#include <cassert>
#include <random>

#include "structures.hpp"
#include "containers.hpp"
#include "clauseallocator.hpp"
#include "comp.hpp"
#include "canonical_key_context.hpp"

namespace GanakInt {

// ---------------------------------------------------------------
// Canonical clause type: structural properties of a clause
// ---------------------------------------------------------------
struct ClauseType {
  unsigned length;
  unsigned num_pos;   // positive non-singleton literals
  unsigned num_neg;   // negative non-singleton literals
  unsigned num_sing;  // singleton literals

  bool operator==(const ClauseType &o) const {
    return length == o.length && num_pos == o.num_pos
        && num_neg == o.num_neg && num_sing == o.num_sing;
  }
};

struct ClauseTypeHash {
  size_t operator()(const ClauseType &t) const {
    size_t h = t.length;
    h = h * 31 + t.num_pos;
    h = h * 31 + t.num_neg;
    h = h * 31 + t.num_sing;
    return h;
  }
};

// ---------------------------------------------------------------
// Global dictionary: clause type -> random hash values
// ---------------------------------------------------------------
class ClauseTypeDictionary {
public:
  ClauseTypeDictionary() : rng_(42) {}

  // Get hash values for a clause type (creates entry if new)
  std::pair<uint64_t, uint64_t> lookup(const ClauseType &type) {
    auto it = dict_.find(type);
    if (it != dict_.end())
      return it->second;

    // Generate two independent random values for this type
    uint64_t h_pos = rng_() | (uint64_t(rng_()) << 32);
    uint64_t h_neg = rng_() | (uint64_t(rng_()) << 32);
    // Ensure non-zero
    if (h_pos == 0) h_pos = 1;
    if (h_neg == 0) h_neg = 1;
    dict_[type] = {h_pos, h_neg};
    return {h_pos, h_neg};
  }

  size_t size() const { return dict_.size(); }

private:
  // DO NOT iterate dict_ in canonical-key compute paths.
  // std::unordered_map iteration order is implementation-defined and
  // nondeterministic across runs; iterating it would break the
  // "bit-identical output within a binary" invariant. Only keyed lookups
  // (find/operator[]) are used here.
  std::unordered_map<ClauseType, std::pair<uint64_t, uint64_t>, ClauseTypeHash> dict_;
  std::mt19937 rng_;
};

// Global instance (shared across all components).
//
// CROSS-BINARY NOTE: the ClauseType->hash mapping is order-of-first-lookup
// dependent (the mt19937 stream is consumed in ClauseType-encounter order),
// which means two different solver binaries (e.g. ganak vs sharpsat-separator)
// will assign different hashes to the same ClauseType, and therefore generate
// different canonical keys for the same CNF. Each binary is individually
// deterministic. See port plan §7 for the content-deterministic seeding
// remedy if cross-binary key sharing is ever needed.
extern ClauseTypeDictionary g_clause_type_dict;

// ---------------------------------------------------------------
// Canonical key.
//
// Identity is a 128-bit hash — `hash` (low half) + `hash_hi` (high
// half), each a different FNV sum over the canonical clauses.
// `operator==` compares only the two 64-bit halves. At ~10^-20
// birthday-collision probability, wrong-answer risk is functionally
// zero. `CanonicalKeyHash` buckets on the low half.
// ---------------------------------------------------------------
struct CanonicalKey {
  uint64_t hash = 0;            // low 64 bits of the 128-bit L2 hash
  uint64_t hash_hi = 0;         // high 64 bits
  unsigned num_vars = 0;        // total active (X_TRI) vars in the sub-component
  unsigned n_in_clauses = 0;    // active vars actually appearing in at least one canonical clause
  unsigned num_clauses = 0;

  bool operator==(const CanonicalKey &other) const {
    return hash == other.hash && hash_hi == other.hash_hi;
  }
};

struct CanonicalKeyHash {
  size_t operator()(const CanonicalKey &k) const { return k.hash; }
};

// ---------------------------------------------------------------
// Build canonical key (declaration — implemented in canonical_key.cpp)
// ---------------------------------------------------------------
//
// All literal/clause state is read through `ctx`:
//   ctx.values           — per-literal TriValue (active-var check)
//   ctx.alloc            — ClauseAllocator, to deref long-clause offsets
//   ctx.watches          — irredundant binary links (BinCl::irred() filtered)
//   ctx.static_wl_labels — preprocessing-time WL labels (may be null)
//   ctx.is_indep         — projection support, required for projected counting
//   ctx.wl_iterations    — WL refinement iteration count
//
// `is_indep` enters the key at WL ITERATION 0 (see the INVARIANT comment in
// the .cpp), so two structurally-equivalent components with different I/Y
// distributions get distinct keys (required for sound projected counting).
//
// `hash_seed` is accepted for interface symmetry with the identity Ts but is
// unused: the canonical key is derived purely from clause structure via the
// (seed-42) clause-type dictionary and fixed FNV constants.
CanonicalKey build_canonical_key(
    const Comp& comp,
    const CanonicalKeyContext& ctx,
    uint64_t hash_seed);

// One-time global WL computation. Runs at preprocessing finish on the
// post-preprocessing formula's incidence/primal structure. Yields one uint64
// label per variable index (1..num_vars; entry 0 unused). Cheap:
// ~O(num_clauses) per WL iteration, run once for the whole solve.
//
// Port note (§4.1): unlike the sharpsat-separator original — which walked a
// flat SENTINEL_LIT-delimited literal_pool from offset 0 to
// original_lit_pool_size — this version iterates `long_irred_cls` directly
// (real ClauseOfs values) and dereferences each via `alloc.ptr(...)`. Binary
// clauses come from `watches`, filtered by BinCl::irred().
std::vector<uint64_t> compute_static_wl_labels(
    uint32_t num_vars,
    const std::vector<ClauseOfs>& long_irred_cls,
    const LiteralIndexedVector<LitWatchList>& watches,
    const LiteralIndexedVector<TriValue>& values,
    const ClauseAllocator& alloc,
    int n_iters);

}  // namespace GanakInt
