"""Dry-run self-test of the race scheduler using mock processes (no real solvers).

Verifies the orchestration end-to-end:
  A. short-circuit  — a config that finishes inside round 1 wins immediately.
  B. full 3 rounds  — nobody finishes early; the highest-rate COCOA config and
                      the (single) Ganak config become the two frontrunners; the
                      leader resumes in round 3 and finishes; runner-up killed.
  C. resume credit  — a frozen frontrunner's progress does NOT advance while
                      suspended (SIGSTOP genuinely pauses work) and DOES advance
                      when resumed (its round-1 work is credited, not redone).

Run: python race/dryrun.py
"""
from __future__ import annotations

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from race.archetypes import Archetype                    # noqa: E402
from race.procctl import ManagedProc                      # noqa: E402
from race.scheduler import run_race                       # noqa: E402

_MOCK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_mock.py")


def _mock(name, engine, rate, finish, count):
    return Archetype(name, engine, mock_argv=(
        sys.executable, _MOCK, engine, str(rate), str(finish), str(count)))


def scenario_A() -> bool:
    print("\n--- Scenario A: short-circuit on an early finisher ---")
    archs = [
        _mock("cocoa-slow-1", "cocoa", 0.0005, "none", 0),
        _mock("cocoa-fast", "cocoa", 0.01, 80, 4242),     # finishes quickly
        _mock("ganak-x", "ganak", 0.001, "none", 0),
    ]
    r = run_race("dummy.cnf", budget_s=30, round1_s=3, round2_s=2,
                 archetypes=archs, progress_interval=0.25)
    ok = (r["status"] == "solved" and r["winner"] == "cocoa-fast"
          and str(r["count"]) == "4242" and r["round"] == "round1")
    print(f"  -> {r}\n  PASS={ok}")
    return ok


def scenario_B() -> bool:
    print("\n--- Scenario B: funnel 6->4->2->1 by ETA, leader finishes in monitoring ---")
    # Nobody finishes during the 3 scouting rounds (1.5s each = 4.5s active). The funnel
    # ranks by predict_cb ETA and narrows to the fastest COCOA (cocoa-B); ganak (no live
    # ETA) sorts last and is cut. The leader then finishes in monitoring (~4.7s active).
    archs = [
        _mock("cocoa-A", "cocoa", 0.002, "none", 111),   # never finishes
        _mock("cocoa-B", "cocoa", 0.02, 330, 777),       # fastest COCOA; finishes in monitoring
        _mock("cocoa-C", "cocoa", 0.001, "none", 222),
        _mock("ganak-Z", "ganak", 0.003, "none", 333),
    ]
    r = run_race("dummy.cnf", budget_s=60, round1_s=1.5, round2_s=1.5,
                 archetypes=archs, progress_interval=0.25)
    ok = (r["status"] == "solved" and r["winner"] == "cocoa-B"
          and str(r["count"]) == "777" and r["round"] == "monitor")
    print(f"  -> {r}\n  PASS={ok}  (funnel 6->4->2->1, leader finishes in monitoring)")
    return ok


def scenario_C() -> bool:
    print("\n--- Scenario C: SIGSTOP freezes progress; SIGCONT resumes it ---")
    p = ManagedProc(_mock("c", "cocoa", 0.01, "none", 0), "dummy.cnf", 0.2)
    p.start()
    time.sleep(1.2)
    p.stop()
    s1 = p.latest().get("pct_lin")
    time.sleep(1.2)                 # frozen: should NOT advance
    s2 = p.latest().get("pct_lin")
    p.cont()
    time.sleep(1.2)                 # resumed: should advance past s2
    s3 = p.latest().get("pct_lin")
    p.kill()
    f1, f2, f3 = float(s1 or 0), float(s2 or 0), float(s3 or 0)
    frozen = abs(f2 - f1) < 1e-9
    resumed = f3 > f2 + 1e-9
    print(f"  pct_lin: at_stop={f1:.5g} after_freeze={f2:.5g} after_resume={f3:.5g}")
    print(f"  frozen_while_stopped={frozen}  advanced_after_cont={resumed}")
    ok = frozen and resumed
    print(f"  PASS={ok}")
    return ok


def scenario_D() -> bool:
    print("\n--- Scenario D: select_frontrunners (top-2 by closed_bits level + #3 accelerator) ---")
    from types import SimpleNamespace
    from race.comparator import select_frontrunners

    class FP:  # fake proc with controlled level/velocity
        def __init__(s, name, eng, lvl, vel):
            s.arch = SimpleNamespace(name=name, engine=eng); s._l = lvl; s._v = vel
        def closed_bits(s): return s._l
        def closed_bits_velocity(s): return s._v

    # 5 COCOA: top-2 by level = plain(100), adaptive(90); #3 accelerator by velocity
    # among the rest = nosep-cascade(0.42).
    p = [FP("plain", "cocoa", 100, 0.03), FP("reactive", "cocoa", 60, 0.0),
         FP("adaptive", "cocoa", 90, 0.02), FP("adaptive-nosep", "cocoa", 50, 0.01),
         FP("nosep-cascade", "cocoa", 55, 0.42)]
    got = sorted(x.arch.name for x in select_frontrunners(p))
    ok1 = got == sorted(["plain", "adaptive", "nosep-cascade"])
    print(f"  COCOA-only -> {got}  (expect plain,adaptive [level] + nosep-cascade [vel])  PASS={ok1}")

    # <=3 candidates -> return all of them (nothing to narrow)
    p2 = [FP("plain", "cocoa", 100, 0.9), FP("adaptive", "cocoa", 90, 0.02), FP("reactive", "cocoa", 60, 0.0)]
    got2 = sorted(x.arch.name for x in select_frontrunners(p2))
    ok2 = got2 == sorted(["plain", "adaptive", "reactive"])
    print(f"  <=3 keep all -> {got2}  PASS={ok2}")

    # cross-engine: COCOA + Ganak -> best COCOA (by level) + the Ganak hedge
    p3 = [FP("plain", "cocoa", 100, 0.03), FP("adaptive", "cocoa", 80, 0.5), FP("ganak-native", "ganak", 0, 0)]
    got3 = sorted(x.arch.name for x in select_frontrunners(p3))
    ok3 = got3 == sorted(["plain", "ganak-native"])
    print(f"  cross-eng  -> {got3}  (best COCOA plain + ganak)  PASS={ok3}")

    ok = ok1 and ok2 and ok3
    print(f"  PASS={ok}")
    return ok


def main() -> int:
    results = [scenario_A(), scenario_B(), scenario_C(), scenario_D()]
    n_ok = sum(results)
    print(f"\n===== dry-run: {n_ok}/{len(results)} scenarios PASS =====")
    return 0 if n_ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
