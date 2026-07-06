"""Step-2 race archetypes: the curated config set raced on UNDECIDED instances.

Each archetype is one (engine, flags) bet on WHAT structure the instance has and
how to exploit it. The set is a COVERING SET over the structural design space:
  {separator usable?} x {branch on var vs clause} x {picker: legacy / adaptive-probe
  / unified} x {BCP-cascade preference?} x {cache capacity}. The 8 COCOA configs:

  cocoa-plain          static sep + legacy picker; clean sep, tiny leaves (cache wins)
  cocoa-adaptive       static sep + adaptive-probe; good sep but DENSE leaves
  cocoa-reactive       static sep + unified picker + reactiveMetis; sep emerges in search
  cocoa-unified-sep    static sep + unified (clause+var, sep-boosted) picker
  cocoa-sep-cascade    static sep + unified picker + BCP-cascade; binary-heavy + sep
  cocoa-cache-max      static sep, wlIter 2 + bigger cache; huge decomposable tree
  cocoa-adaptive-nosep no usable sep + adaptive-probe; dense-small residual
  cocoa-nosep-cascade  no usable sep + unified picker + BCP-cascade; binary-heavy
  ganak-native         the battle-tested fallback engine (handoff, not raced here)

Order does NOT affect selection (the ETA funnel re-ranks regardless); it only sets
short-circuit latency — round 1 runs them in listed order and the race short-circuits
the instant any config FINISHES, so likely-fast configs come early on EASY instances.
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

    # Sound-config flags injected for EVERY COCOA archetype (single point so
    # no config is missed, current or future). The cache-pollution fix
    # (mc2026_169): -cachePurge 1 purges learned-clause-poisoned cache
    # entries on branch-arm failure; -provLocalTaint spares the ~97% of
    # phantom firings that are provably locally entailed (Level-2), so the
    # purge cost stays ~1.19x worst-case / ~0 typical. Makes COCOA sound
    # standalone — the sweep is then sound by construction, with ganak-verify
    # as the belt-and-suspenders net. See cocoa/docs/cache_soundness_fix_plan.md.
    COCOA_SOUND_FLAGS = ("-cachePurge", "1", "-provLocalTaint")

    def argv(self, cnf: str) -> list:
        if self.mock_argv is not None:
            return list(self.mock_argv)
        if self.engine == "cocoa":
            return [cocoa_bin(), *self.flags, *self.COCOA_SOUND_FLAGS, cnf]
        return [ganak_bin(), *self.flags, cnf]

    def env(self, progress_interval: float) -> dict:
        e = dict(os.environ)
        # Real COCOA needs the env toggles to emit PROGRESS lines; Ganak's
        # progress is enabled via its --verb flag (in `flags`). Mocks ignore env.
        if self.mock_argv is None and self.engine == "cocoa":
            e["SHARPSAT_PROGRESS"] = "1"
            e["SHARPSAT_PROGRESS_INTERVAL"] = str(progress_interval)
        return e


# The 8-config COCOA covering set: each occupies a distinct cell of the structural
# design space (separator static/reactive/none, var/clause branching, picker
# legacy/adaptive/unified, BCP-cascade on/off, cache capacity) so the race explores
# the space instead of collapsing to a few points. sep-cascade fills the "separator
# present AND binary-heavy" cell; cache-max fills the "huge decomposable tree, hit
# rate is the bottleneck" cell (wlIter 1->2 collapses more isomorphic components; the
# +1 GB over the 20 GB default is a safe nudge, the canonicalization is the real lever).
# NOTE: no -t here — the scheduler bounds run time via SIGSTOP windows.
STRONG = [
    Archetype("cocoa-plain", "cocoa", ("-sep", "5", "-cb", "3")),
    Archetype("cocoa-adaptive", "cocoa",
              ("-sep", "5", "-cb", "3", "-adaptive", "-wlIter", "2")),
    Archetype("cocoa-reactive", "cocoa",
              ("-sep", "5", "-cb", "3", "-wlIter", "2", "-reactiveMetis",
               "-reactiveMetisMin", "10", "-reactiveMetisSkip", "4",
               "-unifiedPicker", "-decomposeAfterK", "1000", "-cascadeW", "0")),
    Archetype("cocoa-unified-sep", "cocoa",
              ("-sep", "5", "-cb", "3", "-unifiedPicker", "-decomposeAfterK", "1000")),
    # NEW: unified picker + BCP-cascade ON a static separator (= unified-sep + cascade).
    Archetype("cocoa-sep-cascade", "cocoa",
              ("-sep", "5", "-cb", "3", "-unifiedPicker", "-decomposeAfterK", "1000",
               "-cascadeW", "10", "-cascadeDepth", "9")),
    # NEW: cache-max — plain tuned for huge decomposable trees (better canonicalization
    # via wlIter 2 + a safe cache bump to 21 GB). Cheapest (legacy) picker on purpose:
    # in the tiny-leaf regime the picker barely matters, the cache does the work.
    Archetype("cocoa-cache-max", "cocoa",
              ("-sep", "5", "-cb", "3", "-wlIter", "2", "-cs", "21000")),
    Archetype("cocoa-adaptive-nosep", "cocoa", ("-adaptive", "-wlIter", "2")),
    Archetype("cocoa-nosep-cascade", "cocoa",
              ("-unifiedPicker", "-decomposeAfterK", "1000",
               "-cascadeW", "10", "-cascadeDepth", "9")),
    # Native deterministic Ganak (--prob 0, 26 GB). Validated faster than canonical
    # 5/5 on solvable instances (race/hash_study.py, 2026-06-03); the COCOA dive does
    # NOT transfer to Ganak's hash choice, so we just use native everywhere.
    Archetype("ganak-native", "ganak",
              ("--prob", "0", "--maxcache", "15000", "--verb", "1")),
]

# Battle-tested NATIVE Ganak for the COCOA-struggle fallback: deterministic exact
# (--prob 0), 26 GB cache. On t1_101 (all 5 COCOA configs timed out at 180s) this
# solves in 73s — faster than --cachehash canonical (85s) there. Used when a
# COCOA-routed instance's best config is still crawling (<1% pct_lin) after a full
# round: we kill COCOA (free the RAM Ganak needs) and hand it the rest of the budget.
GANAK_FALLBACK = Archetype("ganak-native", "ganak",
                           ("--prob", "0", "--maxcache", "15000", "--verb", "1"))

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
              ("--cachehash", "canonical", "--wliter", "2", "--maxcache", "15000", "--verb", "1")),
]


def default_set() -> list:
    return list(STRONG)


def race_plan(band: str) -> dict:
    """Map the step-1 nd_cost band to a step-2 race plan (config ORDER + Ganak fallback).

    The band NO LONGER PRUNES the candidate set — that over-collapsed us to 2
    points and made the frontrunner narrowing vacuous. We ALWAYS race the full
    8-config covering set (plain/adaptive/reactive/unified-sep/sep-cascade/cache-max/
    adaptive-nosep/nosep-cascade); the band only ORDERS them (likely-winner first, so
    round 1 short-circuits sooner on easy instances). The order does NOT affect the ETA
    funnel's ranking — it matters only for short-circuit latency on easy instances.

    Scheme: 8 configs x round1_s (round 1), narrowed 8->4->2->1 by predict_cb ETA
    over 3 rounds -> the single leader runs to budget, with a monitoring re-check
    that hands to Ganak on sustained overshoot (see scheduler).

    Returns {archs (ordered 8), fallback}.
    """
    (plain, adaptive, reactive, unified_sep, sep_cascade, cache_max,
     adaptive_nosep, nosep_cascade, _ganak) = STRONG
    if band == "low":        # good static separator -> sep/cache configs first
        order = [plain, cache_max, adaptive, unified_sep, sep_cascade, reactive,
                 nosep_cascade, adaptive_nosep]
    elif band == "mid":      # reactive/adaptive often win -> them first
        order = [reactive, adaptive, unified_sep, sep_cascade, plain, cache_max,
                 nosep_cascade, adaptive_nosep]
    elif band == "high":     # mostly hopeless, but sep+cache catches caching (t1_011)
        order = [plain, cache_max, nosep_cascade, sep_cascade, adaptive, reactive,
                 unified_sep, adaptive_nosep]
    else:                    # unknown
        order = [plain, adaptive, reactive, unified_sep, sep_cascade, cache_max,
                 nosep_cascade, adaptive_nosep]
    return {"archs": order, "fallback": GANAK_FALLBACK}
