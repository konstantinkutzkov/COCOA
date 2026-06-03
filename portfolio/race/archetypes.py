"""Step-2 race archetypes: the curated config set raced on UNDECIDED instances.

Each archetype is one (engine, flags) bet on WHY the instance is hard and how to
exploit it (see project_selection_pipeline). The 6 "strong" archetypes are each a
documented winner on some instance in docs/benchmark_log.md:

  cocoa-plain          static recursive separator + raw-count picker     t1_049
  cocoa-reactive       dynamic separators emerge during branching        t1_041 / t1_105
  cocoa-adaptive       dense-regime adaptive branching (sep-gated)       t1_045
  cocoa-adaptive-nosep adaptive when the sep-gate rejects the hierarchy  t1_047
  cocoa-nosep-cascade  no usable separator; score variables deeply       t1_059
  ganak-canonical      the projected/TD full-count engine                cross-solver racer

Order matters: round 1 runs them in listed order and the race short-circuits the
instant any config FINISHES, so likely-fast configs should come early.
"""
from __future__ import annotations

import os
from dataclasses import dataclass

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.normpath(os.path.join(_HERE, "..", ".."))


def cocoa_bin() -> str:
    return os.environ.get("PORTFOLIO_SHARPSAT_BIN",
                          os.path.join(_REPO, "cocoa", "build", "sharpSAT"))


def ganak_bin() -> str:
    return os.environ.get("PORTFOLIO_GANAK_BIN",
                          os.path.join(_REPO, "ganak-canonical", "build", "ganak"))


@dataclass(frozen=True)
class Archetype:
    name: str
    engine: str                  # "cocoa" | "ganak"
    flags: tuple = ()            # solver flags, placed before the cnf
    mock_argv: tuple | None = None   # if set, used verbatim (dry-run only); cnf ignored

    def argv(self, cnf: str) -> list:
        if self.mock_argv is not None:
            return list(self.mock_argv)
        binp = cocoa_bin() if self.engine == "cocoa" else ganak_bin()
        return [binp, *self.flags, cnf]

    def env(self, progress_interval: float) -> dict:
        e = dict(os.environ)
        # Real COCOA needs the env toggles to emit PROGRESS lines; Ganak's
        # progress is enabled via its --verb flag (in `flags`). Mocks ignore env.
        if self.mock_argv is None and self.engine == "cocoa":
            e["SHARPSAT_PROGRESS"] = "1"
            e["SHARPSAT_PROGRESS_INTERVAL"] = str(progress_interval)
        return e


# The 6 strong archetypes (documented winners). NOTE: we do NOT set -t here;
# the scheduler bounds run time via SIGSTOP windows, so a wall-based -t would be
# corrupted by suspension (see project notes).
STRONG = [
    Archetype("cocoa-plain", "cocoa", ("-sep", "5", "-cb", "3")),
    Archetype("cocoa-reactive", "cocoa",
              ("-sep", "5", "-cb", "3", "-wlIter", "2", "-reactiveMetis",
               "-reactiveMetisMin", "10", "-reactiveMetisSkip", "4",
               "-unifiedPicker", "-decomposeAfterK", "1000", "-cascadeW", "0")),
    Archetype("cocoa-adaptive", "cocoa",
              ("-sep", "5", "-cb", "3", "-adaptive", "-wlIter", "2")),
    Archetype("cocoa-adaptive-nosep", "cocoa", ("-adaptive", "-wlIter", "2")),
    Archetype("cocoa-nosep-cascade", "cocoa",
              ("-unifiedPicker", "-decomposeAfterK", "1000",
               "-cascadeW", "10", "-cascadeDepth", "9")),
    # Native deterministic Ganak (--prob 0, 26 GB). Validated faster than canonical
    # 5/5 on solvable instances (race/hash_study.py, 2026-06-03); the COCOA dive does
    # NOT transfer to Ganak's hash choice, so we just use native everywhere.
    Archetype("ganak-native", "ganak",
              ("--prob", "0", "--maxcache", "26000", "--verb", "1")),
]

# Battle-tested NATIVE Ganak for the COCOA-struggle fallback: deterministic exact
# (--prob 0), 26 GB cache. On t1_101 (all 5 COCOA configs timed out at 180s) this
# solves in 73s — faster than --cachehash canonical (85s) there. Used when a
# COCOA-routed instance's best config is still crawling (<1% pct_lin) after a full
# round: we kill COCOA (free the RAM Ganak needs) and hand it the rest of the budget.
GANAK_FALLBACK = Archetype("ganak-native", "ganak",
                           ("--prob", "0", "--maxcache", "26000", "--verb", "1"))

# Optional fillers (slots 7-8), off by default — diminishing returns.
FILLERS = [
    Archetype("cocoa-reactive-dense", "cocoa",
              ("-sep", "5", "-cb", "3", "-wlIter", "2", "-reactiveMetis",
               "-reactiveMetisMin", "10", "-reactiveMetisSkip", "2",
               "-unifiedPicker", "-decomposeAfterK", "1000", "-cascadeW", "0")),
    # TODO: confirm whether `hashed` or `diffpacked` is Ganak's intended fast
    # "identity" analog before promoting this out of fillers.
    Archetype("ganak-identity", "ganak",
              ("--cachehash", "diffpacked", "--verb", "1")),
    # canonical-hash Ganak: an A/B candidate only. Native beat it 5/5 on tested
    # instances; kept as a filler pending evidence it ever wins (hard long runs).
    Archetype("ganak-canonical", "ganak",
              ("--cachehash", "canonical", "--wliter", "2", "--maxcache", "26000", "--verb", "1")),
]


def default_set() -> list:
    return list(STRONG)
