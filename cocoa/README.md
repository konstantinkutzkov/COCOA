# sharpSAT-separator

A #SAT (model counting) solver based on sharpSAT, extended with:

- **Clause branching** via the identity `#SAT(F) = #SAT(F\{C}) − #SAT(F\{C} ∧ ¬C)` for long clauses.
- **METIS-based static separator hierarchy** (ND-hierarchy) computed once from the primal graph and used to guide branching.
- **Two-level component cache** — a 128-bit identity-hash L1 + a canonical-form-hash L2 (compact 128-bit by default; strict mode available for debugging).
- **#SAT-sound preprocessing**: subsumption, pure-duplicate resolution, self-subsuming resolution.
- **Probe-based preprocessing** (opt-in) — the diff-and-lift schema described in [docs/probe_preprocessing_plan.md](docs/probe_preprocessing_plan.md).

## Building

Dependencies: GMP (`gmp` + `gmpxx`), METIS, GKlib. On macOS you can install GMP via Homebrew (the build expects it under `/opt/homebrew/opt/gmp`).

METIS and GKlib are looked up via `METIS_DIR` and `GKLIB_DIR` cmake variables (defaults to `${CMAKE_SOURCE_DIR}/../METIS` and `../GKlib`).

```sh
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DMETIS_DIR=/path/to/METIS \
         -DGKLIB_DIR=/path/to/GKlib
make -j4
```

The Release build adds `-fno-stack-protector`, `-D_FORTIFY_SOURCE=0`, and `-mcpu=native` (the last only on arm64) for ~1–3% speed over plain `-O3`.

**Always verify the build type before benchmarking** — an empty `CMAKE_BUILD_TYPE` silently drops all optimization flags:

```sh
grep CMAKE_BUILD_TYPE build/CMakeCache.txt   # must show =Release
```

## Running

```sh
./build/sharpSAT [options] file.cnf
```

The recommended default invocation for structural / industrial-style instances:

```sh
./build/sharpSAT -rec -sep 5 -cb 3 -sepMode metis file.cnf
```

`-rec` and `-sepMode metis` are accepted for compatibility but are now defaults; the effective flags are `-sep 5 -cb 3`.

## Command-line flags

### General

| Flag | Effect |
|---|---|
| `-q` | Quiet mode (suppresses stats output). |
| `-v` | Verbose mode. |
| `-t SECS` | Time bound in seconds. |
| `-noPP` | Disable preprocessing entirely. |
| `-noCC` | Disable component caching. |
| `-noIBCP` | Disable implicit BCP (failed-literal test during preprocessing). |
| `-noLearn` | Disable conflict-clause learning. |
| `-cs MB` | Maximum cache size in MB. |

### Branching

| Flag | Effect |
|---|---|
| `-cb [N]` | Enable clause branching for clauses of length ≥ N (default N=8). |
| `-sep [N]` | Enable separator branching when active vars ≥ N (default N=15). |
| `-adaptive` | Use τ-based adaptive branching on the no-separator path. |
| `-adaptiveMin N` | Skip adaptive probing for components with fewer than N active vars (default 12). |
| `-adaptiveAlpha F` | Stage-0 length-decay α; default auto-picked from formula density. |
| `-noAutoAlpha` | Disable analyzer-chosen α. |

### Reactive METIS (opt-in, currently regresses on dense instances)

| Flag | Effect |
|---|---|
| `-reactiveMetis` | Enable runtime-METIS fallback at hierarchy-reject points. |
| `-reactiveMetisMin N` | Min active vars to trigger reactive METIS (default 15). |
| `-reactiveMetisSkip K` | After a reactive-METIS failure, wait K decomposition levels before retrying (default 5). |
| `-reactiveMetisBeta B` | Branching-var quality gate: require σ_sep_avg ≥ B·σ_top (default 0.5). |

### Conflict-clause learning

| Flag | Effect |
|---|---|
| `-learnLevel N` | Learning feature ladder. Default 4 (full learning, no minimization). 5=full + rewritten minimize, 3=no bin-pad, 2=no scope, 1=no dedup, 0=no learn. |
| `-verifyLearn` | Replay each minimization's resolution chain for end-to-end sanity check (slow). |

### Implicant learning (opt-in, currently slower than baseline)

| Flag | Effect |
|---|---|
| `-implicantLearn` | Enable implicant learning (scoped clauses from BCP traces). |
| `-implicantMaxSize N` | Max decision literals in a learned implicant (default 4). |
| `-implicantMaxTotal N` | Cap on total implicants learned per solve (default 100000). |

### Standalone preprocessor (`#SAT`-sound simplification)

| Flag | Effect |
|---|---|
| `-noSubsumption` | Disable preprocess subsumption. |
| `-noPureDup` | Disable preprocess pure-duplicate resolution. |
| `-noSSR` | Disable preprocess self-subsuming resolution. |
| `-preprocessBudget MS` | Wall-clock cap for the simplification phase (default 10000). |
| `-preprocessVerbose` | Per-pass stats. |

### Probe-based preprocessing (opt-in)

Runs after the standard preprocessor; uses A as a black box per the [diff-and-lift schema](docs/probe_preprocessing_plan.md). With the strict "must shrink" filter, only candidates that are units, subsume an existing clause, or self-subsume an existing clause are applied.

| Flag | Effect |
|---|---|
| `-localSearchPreprocess` | Enable the pass (default OFF). |
| `-lspMaxProbes N` | Max probes per pass (default 1000). |
| `-lspMaxSize N` | Max σ length (default 4). |
| `-lspMaxTotal N` | Max useful operations applied per invocation (default 5000). |
| `-lspNoR4` | Disable definitional elimination (R4). |
| `-lspVerbose` | Per-pass stats: `units=N subsume=N ssr=N elim=N elapsed_ms=N`. |

### Cache mode (debug aid)

| Flag | Effect |
|---|---|
| `-l2Strict` | L2 cache uses strict canonical keys (128-bit hash + full clause multiset for structural equality). Default is compact (128-bit hash only). Debug aid. |
| `-l2Compact` | Force compact mode (default). |
| `-verifyCache` | Force recomputation on every cache hit and compare against the stored count. Aborts on mismatch. Slow; for catching canonicalization bugs. |

## Tests

Unit and regression tests are built alongside the solver and live under `tests/`:

```sh
cd build
./test_canonical_key_invariance /tmp/some.cnf
./test_canonical_key_learned /tmp/some.cnf
./test_probe_preprocessor_harness
./test_probe_preprocessor_lift
./test_probe_preprocessor_filter
./test_probe_preprocessor_r4
./test_probe
```

The probe-preprocessor tests include a brute-force `#SAT` invariance check over ~50 random small CNFs, which is the strongest soundness signal for the diff-and-lift pass.

## Documentation

Design docs and investigation notes live under [docs/](docs/):

- [docs/probe_preprocessing_plan.md](docs/probe_preprocessing_plan.md) — design of the probe-based preprocessing pass.
- [docs/canonical_caching.md](docs/canonical_caching.md) — content-based component cache design.
- [docs/canonical_key_profiling.md](docs/canonical_key_profiling.md) — profiling of the canonical-key build path.
- [docs/portfolio_driver_plan.md](docs/portfolio_driver_plan.md) — architecture sketch for an automated config selector.
- [docs/portfolio_insights.md](docs/portfolio_insights.md) — per-instance-class observations.
- [docs/benchmark_log.md](docs/benchmark_log.md) — append-only timing record with metadata.
- [docs/nd_hierarchy_orphan_leaf_bug.md](docs/nd_hierarchy_orphan_leaf_bug.md) — closed-bug writeup.
- [docs/t1_011_order_dependent_miscount.md](docs/t1_011_order_dependent_miscount.md) — open-bug investigation.

## Project status

The solver is a research-grade #SAT solver. It is competitive with state-of-the-art on instances where the ND-hierarchy + clause-branching approach exploits structure (verified head-to-head against ganak on `mc2025_track1_049.cnf`: 221 s vs 619 s wall, 234 MB vs 3.1 GB peak). On instances dominated by Arjun-style preprocessing (backbone, equivalence, gate detection), ganak and similar solvers will outperform this one — that's an architectural trade-off, not a bug.
