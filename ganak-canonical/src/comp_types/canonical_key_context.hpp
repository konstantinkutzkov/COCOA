/******************************************
Copyright (C) 2026 Authors of GANAK fork (canonical-key port)
                   Original code Copyright (C) 2023 GANAK Authors

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

#pragma once

#include <cstdint>
#include <vector>

#include "clauseallocator.hpp"
#include "containers.hpp"
#include "structures.hpp"

namespace GanakInt {

// Read-only handle on the literal/clause state needed by the canonical-key
// hasher. The CompManager will own one instance, populated once after
// preprocessing + vivification + subsumption settle (lifecycle landing in
// Commit 4 of the port). The address is threaded through cache calls as
// `const void*` so the identity Ts (HashedComp, DiffPackedComp) can
// continue to ignore it.
//
// Commit 2 (this commit): the struct is defined but never populated.
// CompManager::ctx_ stays default-initialized; dispatch's CANONICAL case
// errors out at startup. Commits 3 and 4 introduce consumers.
//
// is_indep encodes projection support: per-var bit set iff the var is
// in the post-Arjun sampling set (i.e., var index < indep_support_end).
// Used by the canonical-key WL pass at iteration 0 only — see the port
// plan §6 Resolved—Projection.
//
// magic is a runtime tag asserted inside CanonicalKeyComp::set_comp —
// guards against a future maintainer wiring a different ctx struct
// through the void* path.
// NOTE (Commit 4): build_canonical_key's binary-clause walk reads irred binary
// links from a FROZEN snapshot (canon_bin_pool / lit_to_bin_off, built in
// CompAnalyzer::initialize from watches[]) rather than live Counter::watches —
// see the canon_lit_pool note below for why the frozen view is required. The
// identity Ts (HashedComp/DiffPackedComp) ignore the whole context.
// NOTE (Commit 4): canon_lit_pool + clid_to_lit_off are a FROZEN-at-restart-init
// clause-literal snapshot (built in CompAnalyzer::initialize). build_canonical_key
// reads long-clause literals from here, NOT from the live allocator, so the key
// stays consistent with ganak's frozen-at-init component view and is immune to
// mid-restart consolidate (offset rewrites) AND vivify (clause shrink/removal/
// compaction). This replaces the earlier (unsound-under-vivify) live design
// {alloc, long_irred_cls, clid_to_pos}. compute_static_wl_labels receives the
// allocator directly at init time, so no alloc field is needed here.
struct CanonicalKeyContext {
  static constexpr uint32_t MAGIC = 0xCAFEBABEu;

  uint32_t                                  magic            = MAGIC;
  const LiteralIndexedVector<TriValue>*     values           = nullptr;
  // Long-clause frozen snapshot (clause ID -> literals).
  const std::vector<Lit>*                   canon_lit_pool   = nullptr;
  const std::vector<uint32_t>*              clid_to_lit_off  = nullptr;
  // Binary frozen snapshot (lit.raw() -> partner literals). Read instead of live
  // watches[] so the binary view is frozen-at-init like the long-clause one.
  const std::vector<Lit>*                   canon_bin_pool   = nullptr;
  const std::vector<uint32_t>*              lit_to_bin_off   = nullptr;
  const std::vector<uint64_t>*              static_wl_labels = nullptr;
  const std::vector<bool>*                  is_indep         = nullptr;
  int                                       wl_iterations    = 2;
};

}  // namespace GanakInt
