# COCOA

A model-counting (#SAT) solver package combining two complementary engines
behind a selection layer.

## Components

- **`cocoa/`** — the full-counting engine: separator branching (METIS) +
  clause branching + the canonical-key "anonymization" cache. Formerly
  `sharpsat-separator`. Full commit history is preserved.
- **`ganak-canonical/`** — a Ganak fork that is projection-capable and also
  supports the canonical-key cache (`--cachehash canonical`).
- **`portfolio/`** — the selection layer. `route.py` routes an instance to the
  better engine + flags; `ab_compare.py` runs head-to-head configuration A/B
  (e.g. identity vs canonical hashing) with a count-agreement soundness gate.
- **`third_party/`** — vendored METIS + GKlib source (the separator engine's
  dependencies), so a source checkout is self-contained.

## Build

```sh
./build.sh            # builds GKlib -> METIS -> ganak-canonical -> cocoa
```

Build order matters: `cocoa` links METIS/GKlib (from `third_party/`) and the
**static** SAT libraries (cryptominisat5, sbva, cadical, cadiback) produced by
`ganak-canonical`'s build. `build.sh` builds `ganak-canonical` with
`-DBUILD_SHARED_LIBS=OFF` so those deps are static.

Prerequisites: a C/C++ toolchain, CMake, `make`, GMP/GMPXX (and MPFR) — e.g.
`brew install gmp mpfr cmake` on macOS.

> **Network on first build:** `ganak-canonical` uses CMake FetchContent to
> download its SAT dependencies (cryptominisat, cadical, sbva, approxmc, arjun,
> treedecomp) the first time it is configured. A fully offline build would
> require vendoring those sources too (tracked as a follow-up).

Artifacts:
- `cocoa/build/sharpSAT` — the full-counting solver
- `ganak-canonical/build/ganak` — the projection-capable solver

## Usage

```sh
# Auto-route a single instance to the better engine:
portfolio/run.sh input.cnf

# Compare hash/engine configurations head-to-head (soundness-gated):
python3 portfolio/ab_compare.py -t 30 \
    --versions cocoa-identity cocoa-canonical ganak-identity ganak-canonical \
    path/to/cnfs/
```

See `portfolio/versions.py` for the configuration matrix and the per-component
READMEs for engine details.
