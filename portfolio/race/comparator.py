"""Progress comparator for the step-2 race.

Two regimes, very different confidence:

  WITHIN ENGINE (solid).  COCOA configs are compared by `pct_lin` (linear
    completion fraction), tie-broken by `closed_bits` then `decisions`. Ganak
    configs (only relevant if >1 ganak archetype) by a rough work proxy
    (cache entries / cubes / conflicts) — Ganak emits no completion fraction.

  CROSS ENGINE.  Testing (2026-06-03) showed Ganak emits NO live progress: its
    `cache entries K` / `cubes` / `confl/s` block is an END-OF-RUN summary, not
    periodic output (15 s on t1_049 produced only `[consolidate] mini T:` spam).
    So there is nothing to compare a partial Ganak run against. Consequence:
      - Ganak competes ONLY by FINISHING (short-circuit) within its window.
      - If it doesn't finish, defaulting the leader to the COCOA champion is
        FORCED (COCOA's pct_lin is the only real signal), not arbitrary.
    The remaining open problem is therefore NOT cross-engine commensurability but
    WITHIN-COCOA short-window predictiveness: on t1_049, cocoa-adaptive-nosep's
    8 s pct_lin (0.73) outranked cocoa-plain (0.16), yet plain is the winner and
    -adaptive times out >600 s. `within_key` (pct_lin-primary) needs validation.
"""
from __future__ import annotations


def _f(x, default=0.0) -> float:
    try:
        return float(x)
    except (TypeError, ValueError):
        return default


def within_key(engine: str, sample: dict) -> tuple:
    """Higher tuple == more progress, WITHIN one engine."""
    if engine == "cocoa":
        return (_f(sample.get("pct_lin")),
                _f(sample.get("closed_bits")),
                _f(sample.get("decisions")))
    # ganak: no completion fraction — rough "work done" proxy.
    return (_f(sample.get("cache_entries_k")),
            _f(sample.get("cubes_resolved")),
            _f(sample.get("conflicts")))


def within_better(engine: str, a: dict, b: dict) -> bool:
    return within_key(engine, a) > within_key(engine, b)


# ----------------------------------------------------------------------------
# CROSS-ENGINE LEADER PICK.
#
# Default-to-COCOA is FORCED, not a placeholder: Ganak has no live progress (see
# module docstring), so an UNFINISHED Ganak run gives nothing to compare against
# the COCOA champion's pct_lin. Ganak's only path to winning is finishing in its
# window (short-circuit, handled in the scheduler before this is ever called).
# The override-to-Ganak branch below can therefore essentially never fire in
# practice (Ganak's churn fields stay 0 until the end); it is kept only as a
# guard for the hypothetical case where Ganak does emit mid-run churn AND COCOA
# is flat-dead. The real validation debt is in `within_key`, not here.
# ----------------------------------------------------------------------------
def pick_leader(frontrunners: list, log=lambda *_: None):
    """Pick the single leader from the (>=1) frontrunners (one per engine)."""
    by_eng = {p.arch.engine: p for p in frontrunners}

    if len(by_eng) == 1:                          # same engine -> within-engine
        eng = next(iter(by_eng))
        return max(frontrunners, key=lambda p: within_key(eng, p.latest()))

    cocoa = by_eng.get("cocoa")
    ganak = by_eng.get("ganak")
    if cocoa is None:
        return ganak
    if ganak is None:
        return cocoa

    cs = cocoa.latest()
    cp, cb = _f(cs.get("pct_lin")), _f(cs.get("closed_bits"))
    gs = ganak.latest()
    g_cubes, g_confl = _f(gs.get("cubes_resolved")), _f(gs.get("confl_s"))

    cocoa_dead = (cp <= 0.0 and cb <= 0.0)
    ganak_churning = (g_cubes > 0.0 or g_confl > 0.0)
    if cocoa_dead and ganak_churning:
        log(f"[leader] CROSS-ENGINE (v1): COCOA flat (pct_lin={cp:.3g}, "
            f"closed_bits={cb:.3g}); Ganak churning -> ganak")
        return ganak
    log(f"[leader] CROSS-ENGINE (v1, UNVALIDATED): default COCOA "
        f"(pct_lin={cp:.3g}, closed_bits={cb:.3g})")
    return cocoa
