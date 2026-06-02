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

#include "canonical_key_comp.hpp"

#include <cassert>

#include "canonical_key.hpp"
#include "canonical_key_context.hpp"

namespace GanakInt {

uint64_t CanonicalKeyComp::set_comp(const Comp& comp, uint64_t hash_seed,
                                    const BPCSizes& /*bpc*/, const void* ctx_raw) {
  const CanonicalKeyContext* ctx =
      reinterpret_cast<const CanonicalKeyContext*>(ctx_raw);
  assert(ctx && "CanonicalKeyComp requires a non-null CanonicalKeyContext");
  assert(ctx->magic == CanonicalKeyContext::MAGIC &&
         "CanonicalKeyContext magic mismatch -- wrong ctx struct wired through void*?");
  assert(ctx->is_indep && "CanonicalKeyContext::is_indep must be populated");
  assert(ctx->canon_lit_pool && ctx->clid_to_lit_off &&
         ctx->canon_bin_pool && ctx->lit_to_bin_off &&
         "CanonicalKeyContext frozen-snapshot fields must be populated");

  const CanonicalKey key = build_canonical_key(comp, *ctx, hash_seed);
  hi_ = key.hash_hi;
  return key.hash;
}

}  // namespace GanakInt
