"""The step-2 race scheduler.

Sequential, resume-based, under a total wall budget (default 3600 s):

  ROUND 1  run each archetype for `round1_s` (default 120 s); SIGSTOP at the
           window; keep the BEST PER ENGINE (king-of-the-hill, so >=2 frozen +
           1 running resident at a time), KILL the rest.
  ROUND 2  SIGCONT each frontrunner for `round2_s` (default 180 s) more; SIGSTOP.
  LEADER   pick one (the single cross-engine decision; see comparator); KILL the
           runner-up to free memory.
  ROUND 3  SIGCONT the leader; run to completion within the remaining budget.

At ANY point, if a config FINISHES (emits a count) the race short-circuits: that
count is the answer, everything else is killed. Because SIGSTOP credits a frozen
process's work, the leader's round-1+2 time is NOT wasted — it resumes from where
it left off.
"""
from __future__ import annotations

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from race.archetypes import default_set                 # noqa: E402
from race.procctl import ManagedProc                     # noqa: E402
from race.comparator import within_better, pick_leader   # noqa: E402

# run_window outcomes
FINISHED, PAUSED, BUDGET, DIED = "finished", "paused", "budget", "died"


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
                log=_stdout, tag: str = "run", log_every: float = 30.0,
                poll: float = 0.1) -> str:
    """Run an already-started/resumed proc until it finishes, the window elapses,
    or the global deadline hits. Caller is responsible for SIGSTOP on PAUSED.
    Emits a live progress line every `log_every`s so a stuck/odd config is visible
    mid-window (not only at window end)."""
    start = time.monotonic()
    last_log = start
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
        time.sleep(poll)


def _finish(p: ManagedProc, started: list, where: str, log) -> dict:
    cnt = p.count()
    log(f"[FINISH] {p.arch.name} completed in {where}: count={cnt}")
    for q in started:
        if q is not p:
            q.kill()
    p.kill()
    return {"status": "solved", "winner": p.arch.name, "count": cnt,
            "round": where, "active_wall_s": round(p.active_wall(), 1)}


def _kill_all(started: list):
    for q in started:
        q.kill()


def run_race(cnf: str, budget_s: float = 3600.0, round1_s: float = 120.0,
             round2_s: float = 180.0, archetypes=None,
             progress_interval: float = 15.0, log=_stdout) -> dict:
    archetypes = archetypes or default_set()
    deadline = time.monotonic() + budget_s
    started: list[ManagedProc] = []
    champ: dict[str, ManagedProc] = {}   # engine -> best-so-far (suspended)

    # ---- ROUND 1: per-engine king-of-the-hill ----
    for arch in archetypes:
        if time.monotonic() >= deadline:
            log(f"[r1] budget exhausted, skipping {arch.name}")
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
        log(f"[r1] {arch.name}: {_fmt(p.latest())} (active {p.active_wall():.0f}s)")
        eng = arch.engine
        if eng not in champ:
            champ[eng] = p
        elif within_better(eng, p.latest(), champ[eng].latest()):
            champ[eng].kill()
            champ[eng] = p
        else:
            p.kill()
        if out == BUDGET:
            break

    frontrunners = list(champ.values())
    if not frontrunners:
        log("[done] no frontrunners (all died / no budget)")
        _kill_all(started)
        return {"status": "no_result", "count": None}

    # ---- ROUND 2: extend each frontrunner ----
    if len(frontrunners) > 1:
        for p in frontrunners:
            if time.monotonic() >= deadline:
                break
            p.cont()
            log(f"  [r2] {p.arch.name} resumed (window {round2_s:.0f}s)...")
            out = _run_window(p, round2_s, deadline, log, "r2")
            if out == FINISHED:
                return _finish(p, started, "round2", log)
            p.stop()
            log(f"[r2] {p.arch.name}: {_fmt(p.latest())} (active {p.active_wall():.0f}s)")

    # ---- LEADER: the one cross-engine decision ----
    leader = pick_leader(frontrunners, log)
    log(f"[leader] {leader.arch.name}")
    for p in frontrunners:
        if p is not leader:
            p.kill()                          # free memory before round 3

    # ---- ROUND 3: leader to completion ----
    if time.monotonic() < deadline:
        leader.cont()
        log(f"  [r3] {leader.arch.name} resumed to completion "
            f"({deadline - time.monotonic():.0f}s left)...")
        out = _run_window(leader, deadline - time.monotonic() + 1.0, deadline, log, "r3")
        if out == FINISHED:
            return _finish(leader, started, "round3", log)

    log(f"[done] no completion within budget; leader={leader.arch.name}")
    _kill_all(started)
    return {"status": "timeout", "leader": leader.arch.name, "count": None,
            "leader_sample": leader.latest()}


def main(argv: list) -> int:
    ap = argparse.ArgumentParser(description="Step-2 progress-raced portfolio.")
    ap.add_argument("cnf")
    ap.add_argument("--budget", type=float, default=3600.0)
    ap.add_argument("--round1", type=float, default=120.0)
    ap.add_argument("--round2", type=float, default=180.0)
    ap.add_argument("--progress-interval", type=float, default=15.0)
    args = ap.parse_args(argv)
    res = run_race(args.cnf, budget_s=args.budget, round1_s=args.round1,
                   round2_s=args.round2, progress_interval=args.progress_interval)
    print(f"=== race result: {res} ===")
    return 0 if res.get("status") == "solved" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
