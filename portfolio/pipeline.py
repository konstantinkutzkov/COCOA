#!/usr/bin/env python3
"""End-to-end portfolio pipeline: step-1 features -> step-2 race -> model count.

  step 1 (select_solver)  extracts FEATURES (no hard engine verdict):
      nd_cost band (whole ND tree) + Arjun reduction signal.
  step 2 (race.run_race)   uses those features (race.archetypes.race_plan) to pick
      WHICH / HOW MANY COCOA archetypes to race and how eagerly to hand off to
      Ganak, then runs the winner to a count — resuming frontrunner(s) so their
      work is never thrown away. COCOA-vs-Ganak falls out of the race + fallback.

This is the thin driver that connects the two halves; it returns the race result
dict (status / winner / count / round).
"""
from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from select_solver import select_solver                  # noqa: E402
from race.scheduler import run_race, _stdout              # noqa: E402
from race.archetypes import race_plan                     # noqa: E402


def run_pipeline(cnf: str, budget_s: float = 3600.0, round1_s: float = 60.0,
                 progress_interval: float = 10.0, log=_stdout) -> dict:
    # 8-config covering set raced round1_s each, narrowed 8->4->2->1 by predict_cb
    # ETA over 3 rounds -> the single leader runs to budget (with the monitoring
    # Ganak handoff). ~14 min COCOA-only selection on a 1 h budget (round 1 = 8 min).
    d = select_solver(cnf)
    band = d.get("nd_band")
    arj_subst = bool(d.get("arjun_substantial"))
    nd = d.get("nd") or {}
    m = d.get("metis") or {}
    name = os.path.basename(cnf)
    # --- STEP 1: show exactly what METIS produced (root cut + full ND tree) ---
    log(f"[step1] {name} — structural analysis (what METIS produced):")
    log(f"    metis root cut : sep_ratio={m.get('metis_sep_ratio')} "
        f"balance={m.get('metis_balance')} sepsize={m.get('metis_sepsize')} "
        f"n_vars={m.get('metis_n_vars')}")
    log(f"    nd_cost (tree) : band={band} log2_cost={nd.get('nd_log2_cost')} "
        f"root_sep={nd.get('nd_root_sep_vars')} max_path={nd.get('nd_max_path_sep')} "
        f"worst_leaf={nd.get('nd_worst_leaf_vars')} n_leaves={nd.get('nd_n_leaves')}")
    log(f"    arjun signal   : substantial={arj_subst}")

    plan = race_plan(band)
    archs, fallback = plan["archs"], plan["fallback"]
    log(f"[step2] band={band} -> PHASE-2 SOLVERS SELECTED ({len(archs)}): "
        f"{[a.name for a in archs]}  (+Ganak handoff on sustained overshoot)")

    # Per-run forecast trace: every monitoring recheck (~progress_interval s) appends a real
    # time-indexed record (wall_s/active_s/closed_bits/pct_lin/rate/eta/streak) for auditing
    # the handoff and backtesting forecasters. No behaviour change.
    runlogs = os.path.join(os.path.dirname(os.path.abspath(__file__)), "runlogs")
    os.makedirs(runlogs, exist_ok=True)
    base = os.path.splitext(os.path.basename(cnf))[0]
    trace_path = os.path.join(runlogs, f"forecast_{base}.jsonl")
    log(f"[step2] forecast trace -> {trace_path}")

    res = run_race(cnf, budget_s=budget_s, round1_s=round1_s,
                   archetypes=archs, progress_interval=progress_interval, log=log,
                   fallback_archetype=fallback, trace_path=trace_path)
    res["step1_band"] = band
    res["step1_arjun_substantial"] = arj_subst
    res["cnf"] = os.path.basename(cnf)
    return res


def main(argv: list) -> int:
    ap = argparse.ArgumentParser(description="End-to-end #SAT portfolio pipeline.")
    ap.add_argument("cnf")
    ap.add_argument("--budget", type=float, default=3600.0)
    ap.add_argument("--round1", type=float, default=60.0)    # each funnel round
    ap.add_argument("--progress-interval", type=float, default=10.0)
    args = ap.parse_args(argv)
    res = run_pipeline(args.cnf, budget_s=args.budget, round1_s=args.round1,
                       progress_interval=args.progress_interval)
    print(f"\n=== pipeline result: {res} ===")
    if res.get("status") == "solved":
        print(f"COUNT: {res['count']}")
    return 0 if res.get("status") == "solved" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
