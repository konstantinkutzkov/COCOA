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
    """Higher tuple == more progress, WITHIN one engine. COCOA ranks by closed_bits
    (structure resolved) FIRST — pct_lin flatlines at ~0 on deep-tail instances and
    can't discriminate; closed_bits can (see project_progress_signal_and_ganak)."""
    if engine == "cocoa":
        return (_f(sample.get("closed_bits")),
                _f(sample.get("pct_lin")),
                _f(sample.get("decisions")))
    # ganak: no completion fraction — rough "work done" proxy.
    return (_f(sample.get("cache_entries_k")),
            _f(sample.get("cubes_resolved")),
            _f(sample.get("conflicts")))


def within_better(engine: str, a: dict, b: dict) -> bool:
    return within_key(engine, a) > within_key(engine, b)


def select_frontrunners(procs: list, log=lambda *_: None) -> list:
    """Pick the round-2 frontrunners from the round-1 survivors (ManagedProcs).

    COCOA-only race -> the TWO best COCOA configs:
      #1 = highest closed_bits (current leader),
      #2 = highest closed_bits VELOCITY (rising challenger that may overtake);
      tiebreak (same config wins both) -> runner-up by closed_bits.
    This is the fix for COCOA-routed races culling to one config and skipping
    round 2 — it keeps a leader AND an accelerator so crossing trajectories play out.

    Cross-engine race (COCOA + Ganak) -> best COCOA (by closed_bits) + the single
    Ganak hedge (finish-only; no live progress to rank it by)."""
    cocoa = [p for p in procs if p.arch.engine == "cocoa"]
    ganak = [p for p in procs if p.arch.engine == "ganak"]
    if ganak:
        fronts = []
        if cocoa:
            fronts.append(max(cocoa, key=lambda p: p.closed_bits()))
        fronts.append(ganak[0])
        return fronts
    if len(cocoa) <= 2:
        return list(cocoa)
    f1 = max(cocoa, key=lambda p: p.closed_bits())            # leader by level
    f2 = max(cocoa, key=lambda p: p.closed_bits_velocity())   # rising by velocity
    if f2 is f1:                                              # same -> runner-up by level
        f2 = max([p for p in cocoa if p is not f1], key=lambda p: p.closed_bits())
    log(f"[frontrunners] #1 {f1.arch.name} (closed_bits={f1.closed_bits():.1f}) + "
        f"#2 {f2.arch.name} (vel={f2.closed_bits_velocity():.3f} b/s)")
    return [f1, f2]


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

    cb = _f(cocoa.latest().get("closed_bits"))
    cvel = cocoa.closed_bits_velocity()
    gs = ganak.latest()
    g_cubes, g_confl = _f(gs.get("cubes_resolved")), _f(gs.get("confl_s"))

    cocoa_dead = (cvel <= 1e-9)          # closed_bits frozen (reactive-style thrash)
    ganak_churning = (g_cubes > 0.0 or g_confl > 0.0)
    if cocoa_dead and ganak_churning:
        log(f"[leader] CROSS-ENGINE: COCOA frozen (closed_bits_vel={cvel:.3g}, "
            f"closed_bits={cb:.1f}); Ganak churning -> ganak")
        return ganak
    log(f"[leader] CROSS-ENGINE: default COCOA (closed_bits={cb:.1f}, vel={cvel:.3g})")
    return cocoa
