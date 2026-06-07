"""The step-2 race scheduler.

Sequential, resume-based, under a total wall budget (default 3600 s):

  FUNNEL   run each of the 8 configs for `round1_s`; SIGSTOP at the window. After
           each round rank the survivors by predict_cb ETA and narrow 8 -> 4 -> 2
           -> 1, SIGCONT'ing the keepers one more `round1_s` window per stage and
           killing the rest. Three rounds = the COCOA-ONLY evaluation; predict_cb
           is RANKING-only here (no Ganak handoff during scouting).
  MONITOR  SIGCONT the single leader and run it to completion within the remaining
           budget. The ONLY Ganak handoff: re-run predict_cb_arima every R3_RECHECK_EVERY
           s and switch to Ganak after R3_CONSECUTIVE forecasts IN A ROW say
           ETA > R3_OVERSHOOT x the remaining budget (any optimistic forecast resets
           the streak — a between-jumps plateau gets a fair chance before dismissal).

At ANY point, if a config FINISHES (emits a count) the race short-circuits: that
count is the answer, everything else is killed. Because SIGSTOP credits a frozen
process's work, the leader's earlier rounds are NOT wasted — it resumes from where
it left off.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from race.archetypes import default_set                 # noqa: E402
from race.procctl import ManagedProc                     # noqa: E402
from race.forecast import predict_cb as forecast_cb        # noqa: E402  (funnel ranking)
from race.forecast import predict_cb_arima                  # noqa: E402  (monitoring handoff)

# run_window outcomes
FINISHED, PAUSED, BUDGET, DIED, HANDOFF = "finished", "paused", "budget", "died", "handoff"

# Round-3 MONITORING re-check (the ONLY Ganak handoff). After the single leader is
# selected, re-run predict_cb_arima (ARIMA(1,1,0)+drift on closed_bits; inf = stuck-floor
# fired) every R3_RECHECK_EVERY s and hand to Ganak only after R3_CONSECUTIVE forecasts IN
# A ROW say ETA > R3_OVERSHOOT x the remaining budget (any optimistic forecast resets the
# streak). NO handoff during scouting -- COCOA gets its full evaluation, then a debounced
# fair chance, before dismissal. The forecast is microseconds, so re-forecasting at 10s is free.
R3_OVERSHOOT = 1.25
R3_CONSECUTIVE = 10
R3_RECHECK_EVERY = 10.0

# Selection funnel: after each scouting round, keep this many configs (8 -> 4 -> 2 -> 1),
# ranked by predict_cb ETA. Three 1-min rounds = the COCOA-only evaluation window.
# (Initial set size is decoupled: this schedule narrows whatever round 1 produced.)
_FUNNEL_KEEP = (4, 2, 1)

# Round-1 admission gate. Before starting the NEXT round-1 candidate, if the resident
# memory of the already-scouted (now SIGSTOP'd) configs exceeds this, stop admitting --
# don't start any more. We never KILL a running config (no wasted work); we just decline
# to add pressure. Safe because the funnel cuts the set 8->4 next anyway, so an unscouted
# candidate is one we'd likely have dropped. "Better to miss an opportunity than to OOM."
# 20 GB = the 32 GB hard limit minus ~12 GB reserve for the in-flight config's growth
# during its un-checked window (the gate only re-checks BETWEEN configs, so peak RSS ~=
# this threshold + one config's window growth). That growth is small -- cache bytes track
# STORES, a fraction of decisions. Safety over coverage: costs at most ~1 fewer config in
# the moderate-growth case, none in the common low-growth case. The per-config -cs cap is
# the separate backstop against a single config running away mid-window.
R1_MEM_SKIP_GB = 20.0


def _stdout(*a):
    """Flushing logger so output streams live to a redirected file (no buffering)."""
    print(*a, flush=True)


def _fmt(s: dict) -> str:
    if s.get("engine") == "cocoa":
        return (f"pct_lin={s.get('pct_lin', '-')} closed_bits={s.get('closed_bits', '-')} "
                f"decisions={s.get('decisions', '-')}")
    return (f"cache_K={s.get('cache_entries_k', '-')} cubes={s.get('cubes_resolved', '-')} "
            f"confl/s={s.get('confl_s', '-')}")


def _run_window(p: ManagedProc, window_s: float, deadline: float,
                log=_stdout, tag: str = "run", log_every: float = 10.0,
                poll: float = 0.1, recheck=None,
                recheck_every: float = R3_RECHECK_EVERY) -> str:
    """Run an already-started/resumed proc until it finishes, the window elapses,
    or the global deadline hits. Caller is responsible for SIGSTOP on PAUSED.
    Emits a live progress line every `log_every`s (default 10s, real-time view) so
    each config's progress is visible AS IT IS COLLECTED, not only at window end.

    `recheck`, if given, is called every `recheck_every`s as recheck(p, t_remaining);
    if it returns True the window ends with HANDOFF (round-3 forecast bail to Ganak)."""
    start = time.monotonic()
    last_log = start
    last_recheck = start
    while True:
        if p.has_count():
            return FINISHED
        if p.finished():                      # exited; drain so a buffered count
            p.drain()                         # line in the pipe isn't lost to a race
            return FINISHED if p.has_count() else DIED
        now = time.monotonic()
        if now >= deadline:
            return BUDGET
        if now - start >= window_s:
            return PAUSED
        if now - last_log >= log_every:
            log(f"    [{tag}~live] {p.arch.name}: {_fmt(p.latest())} "
                f"(active {p.active_wall():.0f}s)")
            last_log = now
        if recheck is not None and now - last_recheck >= recheck_every:
            last_recheck = now
            if recheck(p, deadline - now):
                return HANDOFF
        time.sleep(poll)


def _finish(p: ManagedProc, started: list, where: str, log) -> dict:
    cnt = p.count()
    log(f"[FINISH] {p.arch.name} completed in {where}: count={cnt}")
    for q in started:
        if q is not p:
            q.kill()
    p.kill()
    return {"status": "solved", "winner": p.arch.name, "decided": p.arch.name,
            "count": cnt, "round": where, "active_wall_s": round(p.active_wall(), 1)}


def _kill_all(started: list):
    for q in started:
        q.kill()


def _group_rss_gb(procs) -> float:
    """Total resident memory (GB) across the procs (SIGSTOP'd ones still count, which is
    the point -- suspended king-of-the-hill configs hold their RAM)."""
    return sum(p.rss_mb() for p in procs) / 1024.0


def _f(x, default=None):
    try:
        return float(x)
    except (TypeError, ValueError):
        return default


def _trace_append(path, rec):
    """Append one JSON record to the per-run forecast trace (best-effort -- a logging
    error must never affect the run). Real time-indexed data: every field is in seconds /
    bits / bits-per-second, sampled at the recheck cadence -- no sample-index quantities."""
    try:
        with open(path, "a") as fh:
            fh.write(json.dumps(rec) + "\n")
    except OSError:
        pass


def _log_rss(procs, log, tag: str):
    """Log total resident memory (GB) across the live process set -- the OOM-relevant
    number. King-of-the-hill keeps non-running configs SIGSTOP'd but RESIDENT, so the
    SUM across the set is what presses on the 32 GB ceiling, not any single cap. This is
    instrumentation only (no enforcement): it makes the cache-growth question answerable
    from logs instead of theory."""
    rows = [(p.arch.name, p.rss_mb()) for p in procs]
    rows = [(n, m) for n, m in rows if m > 0]
    total = sum(m for _, m in rows)
    detail = ", ".join(f"{n}={m / 1024:.1f}G" for n, m in rows) or "none"
    log(f"    [mem~{tag}] total RSS {total / 1024:.1f} GB across {len(rows)} live: {detail}")


def _handoff_to_ganak(fallback, cnf, pi, deadline, started, log, reason) -> dict:
    """Kill all COCOA procs (free the RAM Ganak needs) and run the Ganak fallback
    to completion within the remaining budget. NO fallback-back to COCOA."""
    log(f"[handoff] {reason}")
    log(f"  -> killing COCOA (free RAM), switching to battle-tested {fallback.name}.")
    for q in started:
        q.kill()
    gp = ManagedProc(fallback, cnf, pi)
    gp.start()
    log(f"  [ganak] {fallback.name} to completion "
        f"({deadline - time.monotonic():.0f}s left, 26 GB)...")
    out = _run_window(gp, deadline - time.monotonic() + 1.0, deadline, log, "ganak",
                      log_every=60.0)   # ganak is a black box; per-minute heartbeat
    if out == FINISHED:
        return _finish(gp, [gp], "ganak-handoff", log)
    _kill_all([gp])
    return {"status": "timeout", "leader": fallback.name, "decided": fallback.name,
            "count": None, "leader_sample": gp.latest()}


def _select_by_eta(survivors, keep_n, deadline, log) -> list:
    """Rank survivors by predict_cb ETA (lower = closer to finishing); keep the best
    keep_n. Degenerate/infinite ETAs (deep-tail: closed_bits barely moving) sort last,
    tie-broken by closed_bits LEVEL. RANKING ONLY -- no Ganak handoff here. Non-COCOA
    procs (a Ganak racer, cross-engine tests only) have no live ETA and sort last."""
    t_rem = max(deadline - time.monotonic(), 1.0)
    scored = []
    for p in survivors:
        if p.arch.engine == "cocoa":
            eta = forecast_cb(p.closed_bits_traj(), p.n_root_estimate(),
                              p.active_wall(), t_rem)["eta_s"]
            key = eta if (eta is not None and eta != float("inf")) else float("inf")
        else:
            key = float("inf")
        scored.append((key, -p.closed_bits(), p))
    scored.sort(key=lambda s: (s[0], s[1]))
    kept = [s[2] for s in scored[:keep_n]]
    log(f"[select] keep {keep_n}/{len(survivors)} by predict_cb ETA -> "
        f"{[k.arch.name for k in kept]}")
    return kept


def run_race(cnf: str, budget_s: float = 3600.0, round1_s: float = 60.0,
             archetypes=None, progress_interval: float = 15.0, log=_stdout,
             fallback_archetype=None, trace_path=None) -> dict:
    # FUNNEL: 8 configs x round1_s each, ranked by predict_cb ETA, narrowed 8->4->2->1
    # over 3 rounds (COCOA-only evaluation). The single leader then runs to the budget
    # WITH a monitoring re-check that hands to Ganak only on sustained overshoot.
    archetypes = archetypes or default_set()
    deadline = time.monotonic() + budget_s
    started: list[ManagedProc] = []
    survivors: list[ManagedProc] = []    # all configs that ran round 1 (suspended)

    # ---- ROUND 1: scout every config; select frontrunners after ----
    for i, arch in enumerate(archetypes):
        if time.monotonic() >= deadline:
            log(f"[r1] budget exhausted, skipping {arch.name}")
            break
        # Memory admission gate: don't START a new candidate if the already-scouted
        # (suspended) configs are already using too much RAM. Never kills a running
        # config; just stops admitting. The funnel cuts 8->4 next, so the skipped ones
        # are likely drops anyway -- safer to under-scout than to OOM.
        rss_gb = _group_rss_gb(started)
        if started and rss_gb > R1_MEM_SKIP_GB:
            remaining = [a.name for a in archetypes[i:] if a.engine == "cocoa"]
            log(f"[r1] MEMORY GATE: {rss_gb:.1f} GB resident across {len(started)} configs "
                f"> {R1_MEM_SKIP_GB:.0f} GB cap — NOT admitting the remaining "
                f"{len(remaining)} ({', '.join(remaining)}); the funnel would cut them anyway")
            break
        p = ManagedProc(arch, cnf, progress_interval)
        p.start()
        started.append(p)
        log(f"  [r1] {arch.name} running (window {round1_s:.0f}s)...")
        out = _run_window(p, round1_s, deadline, log, "r1")
        if out == FINISHED:
            return _finish(p, started, "round1", log)
        if out == DIED:
            log(f"[r1] {arch.name} died without a count")
            p.kill()
            continue
        p.stop()
        log(f"[r1] {arch.name}: {_fmt(p.latest())} closed_bits={p.closed_bits():.1f} "
            f"vel={p.closed_bits_velocity():.3f} (active {p.active_wall():.0f}s)")
        survivors.append(p)
        if out == BUDGET:
            break

    if not survivors:
        log("[done] no survivors (all died / no budget)")
        _kill_all(started)
        return {"status": "no_result", "count": None}

    _log_rss(started, log, "after-r1")   # peak concurrency: all scouted configs resident

    # ---- FUNNEL: rank by predict_cb ETA, narrow 8 -> 4 -> 2 -> 1. Ranking ONLY (no
    #      Ganak handoff during scouting). Each survivor gets one more `round1_s` window
    #      per stage; the single leader then enters monitoring (below). ----
    for stage, keep_n in enumerate(_FUNNEL_KEEP):
        if len(survivors) <= 1:
            break
        kept = _select_by_eta(survivors, min(keep_n, len(survivors)), deadline, log)
        for p in survivors:
            if p not in kept:
                p.kill()
        survivors = kept
        if len(survivors) <= 1 or time.monotonic() >= deadline:
            break
        rnd = stage + 2                       # round 1 already ran; extend in r2, r3
        for p in survivors:
            if time.monotonic() >= deadline:
                break
            p.cont()
            log(f"  [r{rnd}] {p.arch.name} resumed (window {round1_s:.0f}s)...")
            out = _run_window(p, round1_s, deadline, log, f"r{rnd}")
            if out == FINISHED:
                return _finish(p, started, f"round{rnd}", log)
            p.stop()
            log(f"[r{rnd}] {p.arch.name}: {_fmt(p.latest())} "
                f"closed_bits={p.closed_bits():.1f} (active {p.active_wall():.0f}s)")
        _log_rss(started, log, f"after-r{rnd}")

    leader = survivors[0]
    log(f"[leader] {leader.arch.name}")

    # NO Ganak handoff during scouting: the funnel (rounds 1-3) evaluates COCOA only --
    # predict_cb there is RANKING-only. The selected leader ALWAYS enters monitoring;
    # COCOA must have a fair shot before being dismissed. The ONLY handoff is the
    # debounced sustained-overshoot re-check during monitoring, below.

    # ---- MONITORING: leader to completion, with the sustained-overshoot re-check ----
    if time.monotonic() < deadline:
        recheck = None
        _handoff_on = fallback_archetype is not None and leader.arch.engine == "cocoa"
        if _handoff_on or trace_path:
            # MONITORING handoff forecaster = predict_cb_arima (ARIMA(1,1,0)+drift on
            # closed_bits, time-indexed in seconds; stuck-floor = inf when flat over the
            # last 90s). eta=inf (stuck) counts as `over`. HANDOFF (only when a fallback is
            # set): hand to Ganak after R3_CONSECUTIVE over-forecasts IN A ROW. NOTE: the
            # ARIMA stuck-floor (90s) and the streak (R3_CONSECUTIVE x 10s) now BOTH debounce
            # in series, so a pure stall bails in ~90s + streak; lower R3_CONSECUTIVE if we
            # want the floor to be the sole debounce. When trace_path is set we ALSO append
            # the live forecast every recheck -- pure data collection, no behaviour change.
            _streak = [0]
            def recheck(p, t_rem):
                fc = predict_cb_arima(p.closed_bits_traj(), p.n_root_estimate(),
                                      p.active_wall(), t_rem)
                eta = fc["eta_s"]
                over = (eta is not None) and (eta > R3_OVERSHOOT * t_rem)  # inf counts as over
                _streak[0] = _streak[0] + 1 if over else 0
                if trace_path:
                    s = p.latest()
                    _trace_append(trace_path, {
                        "phase": "monitor", "config": p.arch.name,
                        "wall_s": round(budget_s - (deadline - time.monotonic()), 1),
                        "active_s": round(p.active_wall(), 1),
                        "closed_bits": _f(s.get("closed_bits")),
                        "pct_lin": _f(s.get("pct_lin")),
                        "decisions": _f(s.get("decisions")),
                        "n_root": fc.get("n_root"),
                        "remaining_bits": fc.get("remaining_bits"),
                        "rate_bits_per_s": fc.get("rate_bits_per_s"),
                        "mu_bits_per_s": fc.get("mu_bits_per_s"),
                        "phi": fc.get("phi"), "stuck": fc.get("stuck"),
                        "eta_s": (eta if (eta is not None and eta != float("inf")) else None),
                        "t_rem_s": round(t_rem, 1),
                        "over": bool(over), "streak": _streak[0],
                    })
                return _handoff_on and over and _streak[0] >= R3_CONSECUTIVE
        leader.cont()
        log(f"  [monitor] {leader.arch.name} resumed to completion "
            f"({deadline - time.monotonic():.0f}s left)...")
        _log_rss([leader], log, "monitor")
        out = _run_window(leader, deadline - time.monotonic() + 1.0, deadline, log, "monitor",
                          log_every=60.0, recheck=recheck)   # full run: per-MINUTE reports
        if trace_path:   # full leader series (scouting + monitoring), real seconds, for fitting
            _trace_append(trace_path, {
                "phase": "leader_full_trajectory", "config": leader.arch.name,
                "outcome": out, "n_root": leader.n_root_estimate(),
                "traj": [{"active_s": round(t, 2), "closed_bits": cb, "pct_lin": pl}
                         for (t, pl, cb) in leader.traj_snapshot()]})
        if out == FINISHED:
            return _finish(leader, started, "monitor", log)
        if out == HANDOFF:
            return _handoff_to_ganak(fallback_archetype, cnf, progress_interval,
                                     deadline, started, log,
                                     f"monitor: {R3_CONSECUTIVE} consecutive forecasts of "
                                     f"ETA > {R3_OVERSHOOT}x remaining (leader walled)")

    log(f"[done] DECISION = {leader.arch.name} (best progress); ran it to the budget "
        f"but it did NOT finish — {_fmt(leader.latest())}. Needs more budget.")
    _kill_all(started)
    return {"status": "timeout", "leader": leader.arch.name,
            "decided": leader.arch.name, "count": None,
            "leader_sample": leader.latest()}


def main(argv: list) -> int:
    ap = argparse.ArgumentParser(description="Step-2 progress-raced portfolio.")
    ap.add_argument("cnf")
    ap.add_argument("--budget", type=float, default=3600.0)
    ap.add_argument("--round1", type=float, default=120.0)
    ap.add_argument("--progress-interval", type=float, default=15.0)
    args = ap.parse_args(argv)
    res = run_race(args.cnf, budget_s=args.budget, round1_s=args.round1,
                   progress_interval=args.progress_interval)
    print(f"=== race result: {res} ===")
    return 0 if res.get("status") == "solved" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
