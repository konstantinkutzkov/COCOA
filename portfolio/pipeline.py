#!/usr/bin/env python3
"""End-to-end portfolio pipeline: step-1 router -> step-2 race -> model count.

  step 1 (select_solver)  narrows the ENGINE FAMILY:
      small METIS separator      -> COCOA  (race the COCOA configs)
      substantial Arjun reduction-> Ganak  (run Ganak; hash via the dive, future)
      neither                    -> UNDECIDED (race ALL archetypes)
  step 2 (race.run_race)   picks the CONFIG within that family and runs it to a
      count, resuming the frontrunner(s) so their work is never thrown away.

This is the thin driver that connects the two halves; it returns the race result
dict (status / winner / count / round).
"""
from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from select_solver import select_solver, COCOA, GANAK    # noqa: E402
from race.scheduler import run_race                       # noqa: E402
from race.archetypes import STRONG                        # noqa: E402

COCOA_ARCHS = [a for a in STRONG if a.engine == "cocoa"]
GANAK_ARCHS = [a for a in STRONG if a.engine == "ganak"]


def run_pipeline(cnf: str, budget_s: float = 3600.0, round1_s: float = 120.0,
                 round2_s: float = 180.0, progress_interval: float = 15.0,
                 log=print) -> dict:
    d = select_solver(cnf)
    solver = d["solver"]
    log(f"[step1] {os.path.basename(cnf)} -> {solver or 'UNDECIDED'}  "
        f"({d['reason'][:100]})")

    if solver == COCOA:
        archs = COCOA_ARCHS
    elif solver == GANAK:
        archs = GANAK_ARCHS
    else:
        archs = STRONG
    log(f"[step2] race {len(archs)} archetype(s): {[a.name for a in archs]}")

    res = run_race(cnf, budget_s=budget_s, round1_s=round1_s, round2_s=round2_s,
                   archetypes=archs, progress_interval=progress_interval, log=log)
    res["step1_solver"] = solver
    res["cnf"] = os.path.basename(cnf)
    return res


def main(argv: list) -> int:
    ap = argparse.ArgumentParser(description="End-to-end #SAT portfolio pipeline.")
    ap.add_argument("cnf")
    ap.add_argument("--budget", type=float, default=3600.0)
    ap.add_argument("--round1", type=float, default=120.0)
    ap.add_argument("--round2", type=float, default=180.0)
    ap.add_argument("--progress-interval", type=float, default=15.0)
    args = ap.parse_args(argv)
    res = run_pipeline(args.cnf, budget_s=args.budget, round1_s=args.round1,
                       round2_s=args.round2, progress_interval=args.progress_interval)
    print(f"\n=== pipeline result: {res} ===")
    if res.get("status") == "solved":
        print(f"COUNT: {res['count']}")
    return 0 if res.get("status") == "solved" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
