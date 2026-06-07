"""Real-binary smoke tests for the race orchestrator (complements dryrun.py).

Runs the ACTUAL sharpSAT / ganak binaries to confirm the things mocks can't:
  T1  real parsers  — COCOA progress+count and Ganak progress+count.
  T2  real suspend  — SIGSTOP freezes a real solver's pct_lin; SIGCONT resumes it.
  T3  short-circuit — the scheduler on an instance the first archetype finishes,
                      returning the correct count through the real pipeline.

These run quick instances. Set PORTFOLIO_CNF_DIR if your CNFs live elsewhere.
"""
from __future__ import annotations

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from race.archetypes import STRONG                       # noqa: E402
from race.procctl import ManagedProc                      # noqa: E402
from race.scheduler import run_race                       # noqa: E402

CNF_DIR = os.environ.get(
    "PORTFOLIO_CNF_DIR",
    "/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/temp_cnf")


def _cnf(n):
    return os.path.join(CNF_DIR, f"mc2025_track1_{n}.cnf")


COCOA_PLAIN = STRONG[0]            # cocoa-plain
GANAK_CANON = STRONG[5]           # ganak-canonical
EXPECT = {
    "071": "456295684783698132731653351484293780287639045166077370506304563500761788632102076272640",
    "159": "2779865831606277536",
    "049": "8695763196077742",
}


def t1_parsers() -> bool:
    print("\n--- T1: real parsers (COCOA t1_071, Ganak t1_159) ---")
    ok = True
    # COCOA: progress + count
    p = ManagedProc(COCOA_PLAIN, _cnf("071"), progress_interval=0.1)
    p.start()
    t0 = time.monotonic()
    while not p.has_count() and not p.finished() and time.monotonic() - t0 < 30:
        time.sleep(0.05)
    p.drain()
    cc = p.count()
    cs = p.latest()
    p.kill()
    cocoa_ok = (cc == EXPECT["071"])
    has_progress = "pct_lin" in cs
    print(f"  COCOA count {'OK' if cocoa_ok else 'MISMATCH'}: {cc}")
    print(f"  COCOA progress sample: {cs}  (pct_lin parsed={has_progress})")
    ok &= cocoa_ok

    # Ganak: progress + count
    g = ManagedProc(GANAK_CANON, _cnf("159"), progress_interval=0.1)
    g.start()
    t0 = time.monotonic()
    while not g.has_count() and not g.finished() and time.monotonic() - t0 < 60:
        time.sleep(0.05)
    g.drain()
    gc = g.count()
    gs = g.latest()
    g.kill()
    ganak_ok = (gc == EXPECT["159"])
    print(f"  Ganak count {'OK' if ganak_ok else 'MISMATCH'}: {gc}")
    print(f"  Ganak progress sample: {gs}")
    ok &= ganak_ok
    print(f"  PASS={ok}")
    return ok


def t2_suspend() -> bool:
    print("\n--- T2: real SIGSTOP freezes / SIGCONT resumes (COCOA t1_049) ---")
    p = ManagedProc(COCOA_PLAIN, _cnf("049"), progress_interval=1.0)
    p.start()
    time.sleep(10)
    p.stop()
    s1 = p.latest().get("pct_lin")
    time.sleep(5)
    s2 = p.latest().get("pct_lin")          # frozen
    p.cont()
    time.sleep(8)
    s3 = p.latest().get("pct_lin")          # advanced
    aw = p.active_wall()
    p.kill()
    f1, f2, f3 = float(s1 or 0), float(s2 or 0), float(s3 or 0)
    frozen = (f1 == f2)
    resumed = (f3 > f2)
    print(f"  pct_lin: at_stop={f1:.6g} after_freeze={f2:.6g} after_resume={f3:.6g}")
    print(f"  active_wall={aw:.1f}s (should be ~18s = 10 run + 8 resumed, NOT 23)")
    print(f"  frozen_while_stopped={frozen}  advanced_after_resume={resumed}")
    ok = frozen and resumed
    print(f"  PASS={ok}")
    return ok


def t3_short_circuit() -> bool:
    print("\n--- T3: scheduler short-circuit on a real fast finisher (t1_071) ---")
    # cocoa-plain (first archetype) finishes t1_071 in <1s -> short-circuit.
    r = run_race(_cnf("071"), budget_s=120, round1_s=20,
                 progress_interval=5.0)
    ok = (r.get("status") == "solved" and r.get("winner") == "cocoa-plain"
          and str(r.get("count")) == EXPECT["071"] and r.get("round") == "round1")
    print(f"  -> {r}")
    print(f"  PASS={ok}")
    return ok


def main() -> int:
    results = [t1_parsers(), t2_suspend(), t3_short_circuit()]
    n = sum(results)
    print(f"\n===== real smoke: {n}/{len(results)} PASS =====")
    return 0 if n == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
