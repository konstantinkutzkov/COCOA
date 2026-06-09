"""The step-2 race scheduler.

Sequential, resume-based, under a total wall budget (default 3600 s):

  FUNNEL   run each of the 8 configs for `round1_s`; SIGSTOP at the window. After
           each round rank the survivors by predict_cb ETA and narrow 8 -> 4 -> 2
           -> 1, SIGCONT'ing the keepers one more `round1_s` window per stage and
           killing the rest. Three rounds = the COCOA-ONLY evaluation; predict_cb
           is RANKING-only here (no Ganak handoff during scouting).
  MONITOR  SIGCONT the single leader and run it to completion within the remaining
           budget. The ONLY Ganak handoff: re-run predict_cb_tree (escape-history
           decision tree) every R3_RECHECK_EVERY s and switch to Ganak after
           R3_CONSECUTIVE 'bail' verdicts IN A ROW (any 'keep' resets the streak — a
           plateau within the config's demonstrated escape envelope keeps its chance).

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
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from race.archetypes import default_set                 # noqa: E402
from race.procctl import ManagedProc                     # noqa: E402
from race.forecast import predict_cb as forecast_cb        # noqa: E402  (round-1 funnel cut)
from race.forecast import predict_cb_recency, predict_cb_tree, RWR_MIN_SPAN_S   # noqa: E402  (cut ranks by recency; handoff decided by the escape-history tree)

# run_window outcomes
FINISHED, PAUSED, BUDGET, DIED, HANDOFF = "finished", "paused", "budget", "died", "handoff"

# Round-3 MONITORING re-check (the ONLY Ganak handoff). After the single leader is
# selected, re-run predict_cb_tree (escape-history decision tree on closed_bits) every
# R3_RECHECK_EVERY s and hand to Ganak only after R3_CONSECUTIVE 'bail' verdicts IN A
# ROW (any 'keep' resets the streak). The streak (10 x 10s) is the SOLE debounce. The
# tree bails a config that has never escaped a plateau (a lone early jump then a wall --
# 017 / hostage) or stalled beyond k x its longest demonstrated escape, but keeps one
# still within its escape envelope (t1_045 recovers from ~3-min plateaus). NO handoff
# during scouting. (The old R3_OVERSHOOT ETA-margin is gone -- the tree decides keep/bail.)
R3_CONSECUTIVE = 10
R3_RECHECK_EVERY = 10.0
# A frontrunner gets a guaranteed minimum of this much ACTIVE running before it can be
# handed off to Ganak (the "3-minute fair chance"). Already satisfied by the round arithmetic
# (60s x 3 rounds = 180s by the time monitoring begins), but made explicit so it holds even
# if round durations change. Scouting counts -- the config ran solo (king-of-the-hill) in each
# window, so it is real dedicated work on the instance.
R3_MIN_ACTIVE_S = 180.0

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


# Grace after the wall deadline before the watchdog force-kills. Caps any overrun at this
# (vs the ~6-9 min mc2026_027 ran past its 60-min budget), while leaving room for the
# normal end-of-budget shutdown (BUDGET return + Ganak-handoff cleanup) to finish first.
WATCHDOG_GRACE_S = 60.0


def _spawn_wall_watchdog(deadline: float, started: list, budget_s: float, log=_stdout,
                         grace_s: float = WATCHDOG_GRACE_S, poll_s: float = 1.0):
    """Enforce the WALL budget independent of the main poll loop. The in-loop
    `now >= deadline` check lives inside _run_window's 0.1s poll, which on a loaded /
    memory-thrashing machine can be starved long past the deadline (mc2026_027 ran ~6-9 min
    over a 60-min budget, still emitting monitor lines, never returning BUDGET). This daemon
    fires `grace_s` after the deadline and force-kills every still-live spawned proc --
    closing their pipes unblocks the reader threads and the stalled main loop, so the run
    ends near budget regardless of scheduling. No-op if the normal path already finished
    (no live procs). Returns the started daemon thread."""
    def _run():
        while time.monotonic() < deadline + grace_s:
            time.sleep(poll_s)
        live = [p for p in started if p.proc is not None and not p.finished()]
        if live:
            log(f"[watchdog] WALL budget + {grace_s:.0f}s grace exceeded "
                f"({budget_s + grace_s:.0f}s); main loop stalled past deadline -- "
                f"force-killing {len(live)} live proc(s): "
                f"{', '.join(p.arch.name for p in live)} "
                f"(max active_wall={max(p.active_wall() for p in live):.0f}s)")
            for p in started:
                p.kill()
    t = threading.Thread(target=_run, daemon=True)
    t.start()
    return t


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
    started.append(gp)   # so the wall-clock watchdog also covers the Ganak fallback
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
    """Rank survivors by ETA (lower = closer to finishing); keep the best keep_n.
    Forecaster is chosen by data span: once a config has >= RWR_MIN_SPAN_S (90s) of
    trajectory it uses predict_cb_recency -- so the round-2 cut (survivors have ~120s / 12
    samples) and the round-3 cut (~180s) rank by recency-weighted rate, while the round-1
    cut (only 60s, below the span gate) falls back to predict_cb. All survivors at a cut
    share the same span, so one forecaster is used per cut. Infinite ETAs (stuck) sort
    same span, so one forecaster is used per cut. Infinite ETAs (stuck / deep-tail) sort
    last, tie-broken by closed_bits LEVEL. RANKING ONLY -- no Ganak handoff here. Non-COCOA
    procs (a Ganak racer, cross-engine tests only) have no live ETA and sort last."""
    t_rem = max(deadline - time.monotonic(), 1.0)
    scored = []
    used = "predict_cb"
    for p in survivors:
        if p.arch.engine == "cocoa":
            traj = p.closed_bits_traj()
            span = (traj[-1][0] - traj[0][0]) if len(traj) >= 2 else 0.0
            if span >= RWR_MIN_SPAN_S:
                fc = predict_cb_recency(traj, p.n_root_estimate(), p.active_wall(), t_rem)
                used = "recency"
            else:
                fc = forecast_cb(traj, p.n_root_estimate(), p.active_wall(), t_rem)
            eta = fc["eta_s"]
            key = eta if (eta is not None and eta != float("inf")) else float("inf")
        else:
            key = float("inf")
        scored.append((key, -p.closed_bits(), p))
    scored.sort(key=lambda s: (s[0], s[1]))
    kept = [s[2] for s in scored[:keep_n]]
    log(f"[select] keep {keep_n}/{len(survivors)} by {used} ETA -> "
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
    _spawn_wall_watchdog(deadline, started, budget_s, log)   # hard WALL cap (see mc2026_027)

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
            # MONITORING handoff forecaster = predict_cb_tree (escape-history decision
            # tree on closed_bits; returns verdict in {'keep','bail'}). verdict=='bail'
            # counts as `over`. HANDOFF (only when a fallback is set): hand to Ganak after
            # R3_CONSECUTIVE 'bail' verdicts IN A ROW -- the SOLE debounce. The tree bails a
            # config that has NEVER escaped a plateau (017 cliff / hostage) and one stalled
            # beyond k x its longest demonstrated escape, but KEEPS a config still within its
            # escape envelope (t1_045 recovers from ~3-min plateaus). Validated keep/bail on
            # 7 traces (KEEP {007,t1_045}; BAIL {011,017,t1_019,t1_031,hostage}). The funnel
            # CUT still ranks by predict_cb_recency; only the handoff uses the tree. When
            # trace_path is set we ALSO append the live forecast every recheck.
            _streak = [0]
            def recheck(p, t_rem):
                nroot = p.n_root_estimate()
                fc = predict_cb_tree(p.closed_bits_traj(), nroot, p.active_wall(), t_rem)
                over = (fc.get("verdict") == "bail")
                _streak[0] = _streak[0] + 1 if over else 0
                if trace_path:
                    s = p.latest()
                    cb = _f(s.get("closed_bits"))
                    _trace_append(trace_path, {
                        "phase": "monitor", "config": p.arch.name,
                        "wall_s": round(budget_s - (deadline - time.monotonic()), 1),
                        "active_s": round(p.active_wall(), 1),
                        "closed_bits": cb,
                        "pct_lin": _f(s.get("pct_lin")),
                        "decisions": _f(s.get("decisions")),
                        "n_root": (nroot if nroot != float("inf") else None),
                        "remaining_bits": ((nroot - cb) if (cb is not None and nroot != float("inf")) else None),
                        "verdict": fc.get("verdict"), "reason": fc.get("reason"),
                        "t_rem_s": round(t_rem, 1),
                        "over": bool(over), "streak": _streak[0],
                    })
                return (_handoff_on and over and _streak[0] >= R3_CONSECUTIVE
                        and p.active_wall() >= R3_MIN_ACTIVE_S)   # 3-min fair chance floor
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
                                     f"monitor: {R3_CONSECUTIVE} consecutive 'bail' verdicts "
                                     f"from the escape-history tree (leader walled)")

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
