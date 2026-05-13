#!/usr/bin/env python3
"""
probe_anchor.py — variable-pinning anchor probe with min-polarity aggregation.

Reproduces and supersedes /tmp/probe_picker.py. Fixes three problems with that
script (each documented in docs/benchmark_log.md 2026-05-12):

  1. Avoids the CNF-renumbering bug. Uses the solver's -forceDecisions flag
     to pin a variable's polarity at the root, leaving the CNF untouched.
     The renumbering approach dropped isolated variables from the active set,
     so F-count + T-count did not equal the unconstrained total.
  2. Probes BOTH polarities and ranks by min(rate_F, rate_T). The earlier
     single-polarity ranking produced ~60% false positives (v70, v176, v33,
     v1 ranked in the top-10 but had one near-TIMEOUT polarity).
  3. Lives in the repo, not /tmp.

Metric (from docs/portfolio_insights.md §4 2026-05-12):

    rate_pol = ( L2 cache hits on sub-components with >= 200 active vars )
               / num_decisions
    score(v) = min(rate_F, rate_T)

Higher rate => more cache amplification during a short fixed-budget run =>
likely faster full solve when this var is pinned as the first decision.

Empirical thresholds on t1_041 (1920 v / 1910 c, ganak-class):
    min_rate >= 0.04  => both polarities solve in < 6 s with high probability
    min_rate <  0.02  => at least one polarity is near-TIMEOUT

Default candidate set is the 2026-05-12 top-10 plus 5 known false positives
plus v24, so the reproduction is one command.
"""

import argparse
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from typing import Optional


REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SOLVER = os.path.join(REPO, "build", "sharpSAT")
DEFAULT_CNF    = os.path.normpath(os.path.join(REPO, "..", "temp_cnf",
                                              "mc2025_track1_041.cnf"))
DEFAULT_FLAGS  = "-rec -sep 5 -cb 3 -sepMode metis -wlIter 2"

# 2026-05-12 top-10 by min-rate + the 5 false positives the old single-polarity
# ranking surfaced + v24 (was rank-4 under the old method).
DEFAULT_VARS = [1, 5, 24, 33, 70, 153, 176, 242, 263, 405, 407, 450, 456, 459,
                526, 631]


@dataclass
class ProbeResult:
    var: int
    polarity: bool          # True = pin v=T (i.e. +v); False = pin v=F (-v)
    decisions: int = 0
    bcp_per_dec: float = 0.0
    h100: int = 0           # L2 hits on components [100, 200)
    h200: int = 0           # L2 hits on components [200, 400)
    h400: int = 0           # L2 hits on components [400, +inf)
    rate: float = 0.0       # (h200 + h400) / decisions
    solved: bool = False    # solver finished within budget
    wall_s: Optional[float] = None
    raw_stderr_tail: str = ""


_DIAG_RE = re.compile(r"DIAG_STATS\s+num_decisions=(\d+)\s+avg_bcp/dec=([\d.eE+-]+)")
_HIST_RE = re.compile(
    r"L2_HIT_HIST\s+\[0-25\)=\d+\s+\[25-50\)=\d+\s+\[50-100\)=\d+\s+"
    r"\[100-200\)=(\d+)\s+\[200-400\)=(\d+)\s+\[400\+\)=(\d+)"
)
_TIME_RE = re.compile(r"^time:\s*([\d.]+)\s*s", re.MULTILINE)
_TIMEOUT_RE = re.compile(r"TIMEOUT\s*!")


def run_probe(solver: str, cnf: str, flags: list, budget_s: float,
              var: int, polarity: bool) -> ProbeResult:
    """One probe: pin (var, polarity), wall-clock budget, parse stats."""
    literal = str(var if polarity else -var)
    cmd = [solver, "-t", str(int(budget_s) if budget_s == int(budget_s)
                              else budget_s)] + flags + [
           "-forceDecisions", literal, cnf]
    # Subprocess timeout = budget + slack for shutdown / stats emission.
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          timeout=max(5.0, budget_s + 3.0))
    out = (proc.stdout or "") + "\n" + (proc.stderr or "")

    r = ProbeResult(var=var, polarity=polarity)
    m = _DIAG_RE.search(out)
    if m:
        r.decisions = int(m.group(1))
        try:
            r.bcp_per_dec = float(m.group(2))
        except ValueError:
            r.bcp_per_dec = 0.0
    m = _HIST_RE.search(out)
    if m:
        r.h100 = int(m.group(1))
        r.h200 = int(m.group(2))
        r.h400 = int(m.group(3))
    r.rate = (r.h200 + r.h400) / max(1, r.decisions)

    # "time:" appears on both clean completion and -t kill. The TIMEOUT
    # marker is the only reliable signal for "did not solve" (memory note
    # 2026-05-12: don't trust time: alone).
    timeout_flagged = bool(_TIMEOUT_RE.search(out))
    m = _TIME_RE.search(out)
    if m:
        r.wall_s = float(m.group(1))
    r.solved = (m is not None) and (not timeout_flagged)

    # Stash a small tail of stderr for diagnostics on weird results.
    err_tail = (proc.stderr or "").splitlines()[-3:]
    r.raw_stderr_tail = " | ".join(err_tail)
    return r


def aggregate(results: list) -> list:
    """Group probe results by var; compute min/max rate across polarities."""
    by_var: dict = {}
    for r in results:
        by_var.setdefault(r.var, {})[r.polarity] = r
    rows = []
    for v, pol_map in by_var.items():
        rF = pol_map.get(False)
        rT = pol_map.get(True)
        if rF is None or rT is None:
            # Only one polarity probed (e.g., user passed --only-pos).
            available = rF if rF is not None else rT
            rows.append({
                "var": v,
                "min_rate": available.rate,
                "max_rate": available.rate,
                "ratio": 1.0,
                "F": rF, "T": rT,
            })
            continue
        min_r = min(rF.rate, rT.rate)
        max_r = max(rF.rate, rT.rate)
        ratio = (max_r / min_r) if min_r > 0 else float("inf")
        rows.append({
            "var": v, "min_rate": min_r, "max_rate": max_r,
            "ratio": ratio, "F": rF, "T": rT,
        })
    rows.sort(key=lambda x: -x["min_rate"])
    return rows


def format_wall(r: Optional[ProbeResult]) -> str:
    if r is None:
        return "    -"
    if r.solved:
        return f"{r.wall_s:6.2f}"
    if r.wall_s is not None:
        return f"T>{r.wall_s:.1f}".rjust(6)
    return "  ???"


def print_table(rows, budget_s):
    print(f"\nRanked by min(rate_F, rate_T) — budget {budget_s}s/probe\n")
    print(f"{'rank':>4} {'var':>5} {'min_rate':>9} {'max_rate':>9} "
          f"{'ratio':>6} {'rate_F':>8} {'rate_T':>8} {'wall_F':>7} {'wall_T':>7} "
          f"{'dec_F':>7} {'dec_T':>7}")
    print("-" * 102)
    for i, row in enumerate(rows, 1):
        rF = row["F"]; rT = row["T"]
        rate_F = f"{rF.rate:.4f}" if rF else "    -"
        rate_T = f"{rT.rate:.4f}" if rT else "    -"
        dec_F  = f"{rF.decisions}" if rF else "-"
        dec_T  = f"{rT.decisions}" if rT else "-"
        print(f"{i:>4} v{row['var']:<4} {row['min_rate']:>9.4f} "
              f"{row['max_rate']:>9.4f} {row['ratio']:>6.2f} "
              f"{rate_F:>8} {rate_T:>8} {format_wall(rF):>7} {format_wall(rT):>7} "
              f"{dec_F:>7} {dec_T:>7}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cnf", default=DEFAULT_CNF,
                    help=f"CNF path (default: {DEFAULT_CNF})")
    ap.add_argument("--solver", default=DEFAULT_SOLVER,
                    help=f"sharpSAT binary (default: {DEFAULT_SOLVER})")
    ap.add_argument("--flags", default=DEFAULT_FLAGS,
                    help=f"Solver flags as one string (default: '{DEFAULT_FLAGS}')")
    ap.add_argument("--budget", type=float, default=1.0,
                    help="Per-probe wall budget in seconds. 0.25 preserves rank "
                         "(2026-05-12); use 1.0 for the reproduction. Default 1.0.")
    ap.add_argument("--vars", default=None,
                    help="Comma-separated variable IDs to probe. Default: the "
                         "2026-05-12 top-10 + 5 false positives + v24.")
    args = ap.parse_args()

    if not os.path.exists(args.solver):
        sys.exit(f"solver not found: {args.solver}")
    if not os.path.exists(args.cnf):
        sys.exit(f"CNF not found: {args.cnf}")

    if args.vars:
        vars_to_probe = sorted({int(x.strip()) for x in args.vars.split(",") if x.strip()})
    else:
        vars_to_probe = sorted(DEFAULT_VARS)

    flags = args.flags.split()
    print(f"probe_anchor: cnf={args.cnf}")
    print(f"              solver={args.solver}")
    print(f"              flags={' '.join(flags)}")
    print(f"              budget={args.budget}s, vars={len(vars_to_probe)} "
          f"x 2 polarities = {len(vars_to_probe)*2} probes")
    print(f"              expected wall: ~{len(vars_to_probe)*2*args.budget:.0f}s")

    results = []
    t0 = time.time()
    for i, v in enumerate(vars_to_probe, 1):
        for polarity in (False, True):
            r = run_probe(args.solver, args.cnf, flags, args.budget, v, polarity)
            results.append(r)
        print(f"  [{i}/{len(vars_to_probe)}] v{v} done ({time.time()-t0:.0f}s)")

    rows = aggregate(results)
    print_table(rows, args.budget)

    # Headline numbers are min-rate-based, not solve-based: at short probe
    # budgets (<= a few seconds), most probes will hit budget without
    # finishing the full solve, so "solved both polarities" would be 0/N
    # and uninformative. Rate is the budget-independent anchor signal.
    # Empirical thresholds from docs/portfolio_insights.md §4 2026-05-12 on
    # t1_041:
    #   min_rate >= 0.04 => both polarities solve in < 6 s w.h.p.
    #   min_rate <  0.02 => at least one polarity is near-TIMEOUT
    n_strong = sum(1 for r in rows[:10] if r["min_rate"] >= 0.04)
    n_weak   = sum(1 for r in rows if r["min_rate"] < 0.02)
    print(f"\nTop-10 with min_rate >= 0.04 (predicted fast both polarities): "
          f"{n_strong}/10")
    print(f"Probed vars with min_rate < 0.02 (predicted slow on >= 1 polarity): "
          f"{n_weak}/{len(rows)}")
    print(f"Total wall: {time.time()-t0:.1f}s")


if __name__ == "__main__":
    main()
