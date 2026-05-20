#!/usr/bin/env python3
"""
14-config variation sweep on t1_023 (102 vars, density 1.00,
the density-1 structured instance where ganak --td 1, ganak --td 0,
and sharpSAT triple-flag all timed out at 600s on 2026-05-19).

Same 14 configs as t1_047_variations.py, 60s budget per config,
PROGRESS captured every 5s. Writes results to
t1_023_variations_results.csv incrementally.

No known model count (no solver has finished t1_023 yet), so the
count_match field is replaced by capturing the count field; if any
config finishes we'll discover the count.
"""
import csv
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path


SHARPSAT = Path("build/sharpSAT").resolve()
TEMP_CNF_DIR = Path("../temp_cnf").resolve()

CNF = "mc2025_track1_023.cnf"
BUDGET_S = 60.0
PROGRESS_INTERVAL_S = 5.0

CONFIGS = [
    ("plain",
     "-rec -sep 5 -cb 3 -sepMode metis"),
    ("plain+react",
     "-rec -sep 5 -cb 3 -sepMode metis -reactiveMetis -reactiveMetisMin 10 "
     "-reactiveMetisSkip 4"),
    ("plain+wl2+react",
     "-rec -sep 5 -cb 3 -sepMode metis -wlIter 2 -reactiveMetis -reactiveMetisMin 10 "
     "-reactiveMetisSkip 4"),
    ("clausesFirst",
     "-rec -sep 5 -cb 3 -sepMode metis -sepClausesFirst"),
    ("legacy_sepVarBias",
     "-rec -sep 5 -cb 3 -sepMode metis -sepVarBias"),
    ("adaptive",
     "-rec -sep 5 -cb 3 -sepMode metis -adaptive"),
    ("picker_vanilla",
     "-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000"),
    ("picker_alpha100",
     "-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000 "
     "-pickerAlphaVar 100 -pickerAlphaClause 100"),
    ("picker_rootSepOnly",
     "-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000 "
     "-pickerRootSepOnly"),
    ("picker_nonSepKillsNd",
     "-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000 "
     "-pickerNonSepKillsNd"),
    ("picker_cascade2",
     "-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000 "
     "-cascadeW 2"),
    ("picker+react",
     "-rec -sep 5 -cb 3 -sepMode metis -reactiveMetis -reactiveMetisMin 10 "
     "-reactiveMetisSkip 4 -unifiedPicker -decomposeAfterK 1000"),
    ("triple_no_lockstep",
     "-rec -sep 5 -cb 3 -sepMode metis -wlIter 2 -reactiveMetis -reactiveMetisMin 10 "
     "-reactiveMetisSkip 4 -unifiedPicker -decomposeAfterK 1000 -cascadeW 0"),
    ("triple+lockstep",
     "-rec -sep 5 -cb 3 -sepMode metis -wlIter 2 -reactiveMetis -reactiveMetisMin 10 "
     "-reactiveMetisSkip 4 -unifiedPicker -decomposeAfterK 1000 -pickerSepLockstep "
     "-cascadeW 0"),
]

COUNT_RE = re.compile(r"^\s*(\d{8,})\s*$", re.MULTILINE)
PROGRESS_RE = re.compile(
    r"PROGRESS\s+t=([\d.eE+-]+)\s+pct_log=[\d.eE+-]+\s+pct_lin=([\d.eE+-]+)\s+"
    r"progress_bits=([\d.eE+-]+)\s+closed_bits=([\d.eE+-]+)\s+open=(\d+)\s+"
    r"bound_log2=([\d.eE+-]+)\s+decisions=(\d+)\s+l2_hits=(\d+)"
)
OPEN_RE = re.compile(
    r"OPEN_WORK\s+n_root=(\d+)\s+n_open_comps=(\d+)\s+"
    r"bound_log2=([\d.eE+-]+)\s+progress_bits=([\d.eE+-]+)"
)
TIME_RE = re.compile(r"^\s*time:\s*([\d.]+)\s*s", re.MULTILINE)


def parse_trajectory(stderr_out: str):
    traj = []
    for m in PROGRESS_RE.finditer(stderr_out):
        traj.append({
            "t": float(m.group(1)),
            "pct_lin": float(m.group(2)),
            "progress_bits": float(m.group(3)),
            "closed_bits": float(m.group(4)),
            "open": int(m.group(5)),
            "bound_log2": float(m.group(6)),
            "decisions": int(m.group(7)),
            "l2_hits": int(m.group(8)),
        })
    return traj


def extract_count(out: str) -> str:
    cands = COUNT_RE.findall(out)
    return max(cands, key=len) if cands else ""


FIELDNAMES = ["config", "finished", "wall_s", "solver_time_s",
              "final_progress_bits", "final_pct_lin", "final_closed_bits",
              "final_decisions", "final_l2_hits",
              "n_root", "n_open_comps", "count", "trajectory_json"]


def append_row(out_path: Path, row: dict, first: bool) -> None:
    mode = "w" if first else "a"
    with open(out_path, mode, newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDNAMES, extrasaction="ignore")
        if first:
            w.writeheader()
        w.writerow(row)


def run_one(name: str, flags: str) -> dict:
    env = os.environ.copy()
    env["SHARPSAT_PROGRESS"] = "1"
    env["SHARPSAT_PROGRESS_INTERVAL"] = str(PROGRESS_INTERVAL_S)
    cmd = ([str(SHARPSAT)] + flags.split() +
           ["-t", f"{BUDGET_S:.1f}", str(TEMP_CNF_DIR / CNF)])
    t0 = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=BUDGET_S + 10.0, env=env)
        wall = time.monotonic() - t0
        stdout = proc.stdout or ""
        stderr = proc.stderr or ""
    except subprocess.TimeoutExpired as e:
        wall = time.monotonic() - t0
        stdout = (e.stdout or b"").decode("utf-8", "replace")
        stderr = (e.stderr or b"").decode("utf-8", "replace")
    out = stdout + "\n" + stderr
    count = extract_count(out)
    finished = bool(count)
    m = TIME_RE.search(out)
    solver_t = float(m.group(1)) if m else None
    m = OPEN_RE.search(stderr)
    n_root = int(m.group(1)) if m else None
    n_open_comps = int(m.group(2)) if m else None
    final_pb = float(m.group(4)) if m else None
    traj = parse_trajectory(stderr)
    final_pct_lin = traj[-1]["pct_lin"] if traj else None
    final_closed = traj[-1]["closed_bits"] if traj else None
    final_dec = traj[-1]["decisions"] if traj else None
    final_hits = traj[-1]["l2_hits"] if traj else None
    return {
        "config": name,
        "finished": finished,
        "wall_s": round(wall, 3),
        "solver_time_s": solver_t,
        "final_progress_bits": final_pb,
        "final_pct_lin": final_pct_lin,
        "final_closed_bits": final_closed,
        "final_decisions": final_dec,
        "final_l2_hits": final_hits,
        "n_root": n_root,
        "n_open_comps": n_open_comps,
        "count": count,
        "trajectory_json": json.dumps(traj),
    }


def main():
    if not SHARPSAT.exists():
        sys.exit(f"sharpSAT not found: {SHARPSAT}")
    out_path = Path(__file__).parent / "t1_023_variations_results.csv"
    print(f"t1_023 variations: {len(CONFIGS)} configs × {BUDGET_S:.0f}s = "
          f"{len(CONFIGS) * BUDGET_S:.0f}s max wall", flush=True)
    t_start = time.monotonic()
    for i, (name, flags) in enumerate(CONFIGS):
        print(f"  [{i+1}/{len(CONFIGS)}] {name} ...", end=" ", flush=True)
        row = run_one(name, flags)
        stat = "SOLVE" if row["finished"] else "TIMEOUT"
        pb = row["final_progress_bits"]
        pb_str = f"{pb:.4g}" if pb is not None else "?"
        pct = row["final_pct_lin"]
        pct_str = f"{pct:.4g}%" if pct is not None else "?"
        cb = row["final_closed_bits"]
        cb_str = f"{cb:.2f}" if cb is not None else "?"
        dec = row["final_decisions"]
        print(f"{stat} pb={pb_str} pct_lin={pct_str} closed_bits={cb_str} "
              f"dec={dec}", flush=True)
        append_row(out_path, row, first=(i == 0))
    elapsed = time.monotonic() - t_start
    print(f"\nDone in {elapsed:.1f}s. Results: {out_path}", flush=True)


if __name__ == "__main__":
    main()
