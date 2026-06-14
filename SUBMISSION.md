# MC 2026 Submission — Checklist & Notes (Track 1, exact MC)

Requirements source: https://mccompetition.org/2026/submission and the format
document `mccomp_format_25.pdf`. Limits: **3600 s**, **32 GB**.

## What's in place (this repo)

| Requirement | Status | Where |
|---|---|---|
| Top-level `build.sh`, no args | ✅ | `build.sh` (GKlib → METIS → GANAK → COCOA; best-effort apt deps) |
| Top-level `run.sh <instance>`, one arg | ✅ | `run.sh` (drives the validated pipeline) |
| Output to stdout in MC2026 format | ✅ | `run.sh` emits `s … / c s type mc / c s log10-estimate … / c s exact arb int N` |
| Reads/handles the `c t mc` type line | ✅ | `run.sh` type-line guard (non-`mc` → format error) |
| UNSAT / count 0 | ✅ | `s UNSATISFIABLE` + `c s log10-estimate -inf` + `c s exact arb int 0` |
| Timeout / no count | ✅ | `s UNKNOWN` |
| 32 GB memory | ✅ | funnel admits to a 20 GB gate; GANAK 26 GB runs only post-handoff (never coexist) |
| 3600 s | ✅ | `run.sh` passes `--budget 3600` |
| System-description document | ✅ (draft) | `SYSTEM_DESCRIPTION.md` |

Verified locally: `run.sh test/tiny.cnf` → `c s exact arb int 7`; instance 075 →
`6905169454`; spec Example 1 (N=22) → `c s log10-estimate 1.342422680822206`
exactly. stdout carries **only** `s`/`c s` lines (full trace → stderr).

## Build-test on the official image — ✅ DONE (passed)

Built + smoke-tested on `registry.gitlab.com/sosy-lab/benchmarking/competition-scripts/user:latest`
(Ubuntu 24.04, amd64) under a 32 GB limit:
```
docker build --platform linux/amd64 -f docker/Dockerfile.build-test -t cocoa-mc2026 .
docker run --rm -m 32g --platform linux/amd64 cocoa-mc2026 ./run.sh test/tiny.cnf
#   -> s SATISFIABLE / c s type mc / c s log10-estimate ... / c s exact arb int 7
```
This shook out four issues (all fixed; commit `e029619`): missing apt deps
(pkg-config/mpfr/numpy), floating ganak FetchContent tags (pinned 7 SHAs — Arjun
HEAD had broken `main.cpp`), static-binary vs Ubuntu's `libflint.so`-only +
broken `flint.pc` (→ `-DSTATIC_BINARY=OFF` + dynamic GMP/MPFR/flint + `.pc`
patch), and cocoa gcc-13 missing transitive includes (force-include shim).

## Manual steps left for you

1. **Build-step network IS required** and works on the image — ganak's
   FetchContent clones its SAT deps (pinned SHAs) during `build.sh`. If the
   competition's build phase is network-isolated, tell me and we'll vendor them.
2. **Confirm the memory limit is 32 GB** (the MC2026 page says 32 GB; the SoSy
   scripts' default *example* shows 15 GB). If it is actually 15 GB, lower GANAK
   `--maxcache` and the funnel gate in `portfolio/race/archetypes.py` /
   `scheduler.py`. (The smoke test passed at `-m 32g`.)
3. **Make the GitHub repo private** and grant the organizers access (rules
   require a private repo).
4. **Push** the final main branch (`origin/main`), then **email the commit hash**
   + `SYSTEM_DESCRIPTION.md` to **mcw@modelcounting.org**.

## Notes

- Submitting to **Track 1 only** (exact unweighted). The solver does not target
  weighted/projected/algebraic tracks; `run.sh` rejects those type lines.
- `portfolio/run.sh` is an *internal* entry (auto-router); the **top-level
  `run.sh`** is the competition entry.
