"""Dry-run self-test of the race scheduler using mock processes (no real solvers).

Verifies the orchestration end-to-end:
  A. short-circuit  — a config that finishes inside round 1 wins immediately.
  B. ETA funnel     — nobody finishes early; the funnel ranks survivors by
                      predict_cb ETA and narrows 6->4->2->1 to the fastest COCOA
                      (ganak, no live ETA, sorts last and is cut); the single
                      leader then finishes in monitoring.
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
    r = run_race("dummy.cnf", budget_s=30, round1_s=3,
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
    r = run_race("dummy.cnf", budget_s=60, round1_s=1.5,
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


def main() -> int:
    results = [scenario_A(), scenario_B(), scenario_C()]
    n_ok = sum(results)
    print(f"\n===== dry-run: {n_ok}/{len(results)} scenarios PASS =====")
    return 0 if n_ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
