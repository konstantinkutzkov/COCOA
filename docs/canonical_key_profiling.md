# Canonical key `operator==` profiling — measurement result

## Context

During repeated discussions of the canonical-cache Phase B roadmap
(see `canonical_cache_phase_b_plan.md`) the idea keeps resurfacing to
replace the full stored-clause-multiset equality check with a
per-clause-hash alternative. The premise is that the full equality
walk (O(Σ|clause|) per cache hit) is a hot path, and swapping it for
a sorted `vector<uint64_t>` would speed up cache lookups.

This document pins the measurement so we don't rediscuss it.

## Method

Ran the solver on `t1_011.cnf` (the most cache-intensive instance in
our current test set — 321,106 cache hits on a 25–45 second run,
over 100k cache stores, no separator branching rejected by the
threshold gates).

Instrumented `CanonicalKey::operator==` in `canonical_key.h` with a
wall-clock accumulator and a call counter. Reverted after the
measurement.

Command:

```
./build/sharpSAT -rec -sep 5 -cb 3 -sepMode metis /tmp/t1_011.cnf
```

## Measurement

Single run, post-instrumentation:

| metric                           | value           |
|----------------------------------|-----------------|
| total solve time                 | 42.85 s         |
| `operator==` total calls         | 321,106         |
| trivial rejects (hash mismatch)  | 0               |
| content compares (full equal)    | 321,106         |
| literals compared (cumulative)   | 121,301,556     |
| total ns in `operator==`         | 221,795,325     |
| total sec in `operator==`        | 0.22 s          |
| **percent of solve time**        | **0.52 %**      |

All 321k calls reached the content-equality check, since `hash`
matched every time. That is expected — `std::unordered_map` only
invokes `operator==` when it has a bucket-level hash match.

## Interpretation

- `operator==` is 0.5 % of solve time on the most cache-intensive
  instance we have. On sparser / less cache-heavy instances it would
  be even less.
- The per-clause-hash optimization's theoretical speedup is roughly
  a 4× reduction in the comparison cost — about 0.14 s saved out of
  42.85 s, i.e. 0.35 % of solve time.
- The memory saving is real (~8× shrink of the stored clause
  representation) but cache eviction is not observed as an active
  pressure on any current test instance.
- The collision-risk tradeoff is not favourable at 0.35 % upside.

## Decision: do not implement per-clause-hash storage

The current design (full stored-clause-multiset equality check after
a hash bucket match) is kept. Rationale pinned here so this is not
revisited without new measurement evidence.

The door is left open: if a future workload shows cache-memory
pressure measured as a concrete bottleneck (e.g. evictions
correlating with solve-time blowup, or RSS ceiling-pressure
blocking larger instances), reopen this analysis with that data.

## What to measure before reopening

Conditions that would justify revisiting the per-clause-hash idea:

1. A workload where `CanonicalKey::operator==` is > 5 % of solve
   time on representative instances.
2. A workload where cache-store size (`ContentCache::cache_`) is
   large enough that eviction frequency correlates with solve-time
   regression.
3. A workload where RSS is the limiting factor on what problem
   sizes the solver can handle.

If any of those show up, the measurement protocol in this document
is the template — add instrumentation, run, record, decide.

Until then, the current architecture is fine.

## Regression tests that remain valuable

Independent of this measurement, both canonical-key regression
tests retain their value:

- `test_canonical_key_invariance` — key unchanged under stored-literal
  permutations.
- `test_canonical_key_learned` — key unchanged when learned clauses
  (long or binary) are injected.

Neither is affected by this profiling outcome; both should stay in
CI and be run whenever canonical-key code is touched.
