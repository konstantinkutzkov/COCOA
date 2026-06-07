#!/usr/bin/env python3
"""Calibration sweep for the nd_cost router signal.

Runs cocoa/build/nd_cost on each base track1 instance, parses nd_log2_cost
(+ diagnostics), attaches any known solve outcome from sweep_log.jsonl, and
prints a table sorted by cost. Writes nd_calibrate.jsonl.

PROVISIONAL: this is a first-pass calibration. The gate threshold derived
here needs far more outcome data (and the cost model ignores caching/BCP, so
absolute bits overestimate) before it can be trusted as the router's gate.

Usage: python nd_calibrate.py [cnf_dir]
"""
from __future__ import annotations

import glob
import json
import os
import re
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_NDCOST = os.path.normpath(os.path.join(_HERE, "..", "cocoa", "build", "nd_cost"))
_DEFAULT_CNF_DIR = "/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/temp_cnf"
_BASE_RE = re.compile(r"^mc2025_track1_(\d+)\.cnf$")  # unsuffixed base instances


def _run_nd_cost(cnf: str) -> dict:
    out = {}
    try:
        p = subprocess.run([_NDCOST, cnf], capture_output=True, text=True, timeout=120)
    except (subprocess.TimeoutExpired, OSError) as e:
        return {"nd_status": f"runner_error:{e}"}
    for line in p.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            try:
                out[k] = float(v) if "." in v else int(v)
            except ValueError:
                out[k] = v
    return out


def _load_outcomes() -> dict:
    """inst -> short outcome string, from sweep_log.jsonl if present."""
    outc = {}
    swp = os.path.join(_HERE, "sweep_log.jsonl")
    if os.path.exists(swp):
        for line in open(swp):
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                continue
            inst = d.get("inst")
            if not inst:
                continue
            status = d.get("status")
            decided = d.get("decided")
            outc[inst] = f"{status}/{decided}" if status else "?"
    return outc


def main(argv) -> int:
    cnf_dir = argv[0] if argv else _DEFAULT_CNF_DIR
    if not os.path.exists(_NDCOST):
        print(f"ERROR: nd_cost not built at {_NDCOST}", file=sys.stderr)
        return 2
    files = sorted(f for f in os.listdir(cnf_dir) if _BASE_RE.match(f))
    outcomes = _load_outcomes()
    rows = []
    for fn in files:
        num = _BASE_RE.match(fn).group(1)
        inst = f"t1_{num}"
        d = _run_nd_cost(os.path.join(cnf_dir, fn))
        d["inst"] = inst
        d["outcome"] = outcomes.get(inst, "")
        rows.append(d)
        print(f"  ran {inst}: nd_log2_cost={d.get('nd_log2_cost')} status={d.get('nd_status')}",
              flush=True)

    rows.sort(key=lambda r: (r.get("nd_log2_cost") if isinstance(r.get("nd_log2_cost"), (int, float)) else 1e18))

    with open(os.path.join(_HERE, "nd_calibrate.jsonl"), "w") as fh:
        for r in rows:
            fh.write(json.dumps(r) + "\n")

    print("\n=== nd_cost calibration (sorted by cost) ===")
    print(f"{'inst':8} {'log2_cost':>9} {'maxpath':>8} {'rootsep':>7} "
          f"{'worstleaf':>9} {'nvars':>6} {'acct':>4}  outcome")
    for r in rows:
        lc = r.get("nd_log2_cost")
        lc_s = f"{lc:.1f}" if isinstance(lc, (int, float)) else str(lc)
        print(f"{r.get('inst',''):8} {lc_s:>9} "
              f"{str(r.get('nd_max_path_sep','')):>8} {str(r.get('nd_root_sep_vars','')):>7} "
              f"{str(r.get('nd_worst_leaf_vars','')):>9} {str(r.get('nd_n_vars','')):>6} "
              f"{str(r.get('nd_acct_ok','')):>4}  {r.get('outcome','')}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
