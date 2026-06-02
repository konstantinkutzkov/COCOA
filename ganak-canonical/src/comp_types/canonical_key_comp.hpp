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
#include "comp.hpp"

namespace GanakInt {

// The canonical-key counterpart to HashedComp / DiffPackedComp (the third
// CacheableComp<T> backend, selected by --cachehash canonical).
//
// Layout: the 128-bit canonical key splits into
//   - low 64 bits  -> returned from set_comp, stored as CacheableComp::hashkey
//   - high 64 bits -> stored here in hi_, compared in equals()
// Together that is full 128-bit identity (CacheableComp::equals already checks
// the low half via hashkey). Birthday-collision probability for the pair at
// 1e9 entries is ~1e-20 -- below ganak's existing identity-cache failure modes.
//
// set_comp is a NON-static member (mirrors DiffPackedComp): CacheableComp<T>
// privately inherits T and calls T::set_comp on `this`, so set_comp can write
// hi_ on the T base subobject and return the low half. The const void* ctx is
// the CanonicalKeyContext (threaded through cache->init / create_new_comp);
// the impl casts + magic-checks it. comp_bytes() returns 0: hi_ lives inside
// the cache slot, already tracked by compute_size_allocated -- returning
// sizeof(*this) would double-count.
class CanonicalKeyComp {
 public:
  CanonicalKeyComp() = default;

  uint64_t set_comp(const Comp& comp, uint64_t hash_seed,
                    const BPCSizes& bpc, const void* ctx_raw);

  bool     equals(const CanonicalKeyComp& other) const { return hi_ == other.hi_; }
  uint64_t comp_bytes() const { return 0; }
  void     set_free()         { hi_ = 0; }

 private:
  uint64_t hi_ = 0;
};

}  // namespace GanakInt
