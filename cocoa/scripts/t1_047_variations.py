#!/usr/bin/env python3
"""
Variations on t1_047 (the density-1 structured 80-var instance where
ganak --td 1 takes 481s and our triple-flag timed out at 600s with
progress_bits=0.57).

Configs (5 total):
  1. plain                    - the no-picker baseline
  2. picker+lockstep          - lockstep restores plain on sep phase
  3. picker+lockstep+react    - + reactiveMetis (dynamic cuts after sep)
  4. triple_no_lockstep       - the original 2026-05-18 t1_041 winner
  5. triple+lockstep          - lockstep + reactiveMetis + wlIter=2

Each runs for 60 seconds. We capture the FINAL OPEN_WORK snapshot
(progress_bits and other fields) plus a trajectory of per-tick PROGRESS
lines (every 5 seconds) so we can see HOW progress accrued, not just
where it ended.

Writes results to t1_047_variations_results.csv (one row per config,
trajectory stored as a JSON string).
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

CNF = "mc2025_track1_047.cnf"
BUDGET_S = 60.0
PROGRESS_INTERVAL_S = 5.0  # SHARPSAT_PROGRESS_INTERVAL

CONFIGS = [
    # --- No-picker family: vary sep order and structural support ---
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

    # --- Picker family: vary the qualitative behavior, not just numbers ---
    # 7. Vanilla picker (no lockstep, no extras) — picker fully overrides
    #    precomputed cut, scoring at default α_var=15. Expect "scattered" picks.
    ("picker_vanilla",
     "-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000"),
    # 8. High-α picker — forces sep elements to dominate scoring (but
    #    still picks by activity within sep, not METIS order).
    ("picker_alpha100",
     "-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000 "
     "-pickerAlphaVar 100 -pickerAlphaClause 100"),
    # 9. Picker + rootSepOnly — picker only honors sep at root; below
    #    that, pure scoring (no sep boost). Tests deep-sep value.
    ("picker_rootSepOnly",
     "-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000 "
     "-pickerRootSepOnly"),
    # 10. Picker + nonSepKillsNd — non-sep picks lose ND-hierarchy descent.
    ("picker_nonSepKillsNd",
     "-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000 "
     "-pickerNonSepKillsNd"),
    # 11. Picker + cascadeW=2 — BCP-cascade signal active (2026-05-09
    #     sweet spot for the multiplicative picker on t1_071/t1_011).
    ("picker_cascade2",
     "-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000 "
     "-cascadeW 2"),
    # 12. Picker + react (= probe_flags' "react-default"). Picker
    #     overrides cut + reactive supplies dynamic cuts.
    ("picker+react",
     "-rec -sep 5 -cb 3 -sepMode metis -reactiveMetis -reactiveMetisMin 10 "
     "-reactiveMetisSkip 4 -unifiedPicker -decomposeAfterK 1000"),
    # 13. Triple flag, no lockstep — yesterday's t1_041 winner (3.04s
    #     on t1_041; 605s TIMEOUT progress_bits=0.57 on t1_047).
    ("triple_no_lockstep",
     "-rec -sep 5 -cb 3 -sepMode metis -wlIter 2 -reactiveMetis -reactiveMetisMin 10 "
     "-reactiveMetisSkip 4 -unifiedPicker -decomposeAfterK 1000 -cascadeW 0"),
    # 14. Triple + lockstep — restore plain on sep phase, picker after.
    ("triple+lockstep",
     "-rec -sep 5 -cb 3 -sepMode metis -wlIter 2 -reactiveMetis -reactiveMetisMin 10 "
     "-reactiveMetisSkip 4 -unifiedPicker -decomposeAfterK 1000 -pickerSepLockstep "
     "-cascadeW 0"),
]

COUNT_RE = re.compile(r"^\s*(\d{8,})\s*$", re.MULTILINE)
TIME_RE = re.compile(r"^\s*time:\s*([\d.]+)\s*s", re.MULTILINE)
PROGRESS_RE = re.compile(
    r"PROGRESS\s+t=([\d.eE+-]+)\s+pct_log=[\d.eE+-]+\s+pct_lin=([\d.eE+-]+)\s+"
    r"progress_bits=([\d.eE+-]+)\s+closed_bits=([\d.eE+-]+)\s+open=(\d+)\s+"
    r"bound_log2=([\d.eE+-]+)\s+decisions=(\d+)\s+l2_hits=(\d+)"
)
OPEN_RE = re.compile(
    r"OPEN_WORK\s+n_root=(\d+)\s+n_open_comps=(\d+)\s+"
    r"bound_log2=([\d.eE+-]+)\s+progress_bits=([\d.eE+-]+)"
)


def parse_trajectory(stderr_out: str):
    """Return a list of dicts, one per PROGRESS line."""
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
              "final_progress_bits", "final_pct_lin", "final_decisions",
              "final_l2_hits", "n_root", "n_open_comps", "trajectory_json"]


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
    # OPEN_WORK final snapshot (in stderr at exit)
    m = OPEN_RE.search(stderr)
    n_root = int(m.group(1)) if m else None
    n_open_comps = int(m.group(2)) if m else None
    final_pb = float(m.group(4)) if m else None
    traj = parse_trajectory(stderr)
    final_pct_lin = traj[-1]["pct_lin"] if traj else None
    final_dec = traj[-1]["decisions"] if traj else None
    final_hits = traj[-1]["l2_hits"] if traj else None
    return {
        "config": name,
        "finished": finished,
        "wall_s": round(wall, 3),
        "solver_time_s": solver_t,
        "final_progress_bits": final_pb,
        "final_pct_lin": final_pct_lin,
        "final_decisions": final_dec,
        "final_l2_hits": final_hits,
        "n_root": n_root,
        "n_open_comps": n_open_comps,
        "trajectory_json": json.dumps(traj),
    }


def append_row(out_path: Path, row: dict, first: bool) -> None:
    mode = "w" if first else "a"
    with open(out_path, mode, newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDNAMES, extrasaction="ignore")
        if first:
            w.writeheader()
        w.writerow(row)


def main():
    if not SHARPSAT.exists():
        sys.exit(f"sharpSAT not found: {SHARPSAT}")
    out_path = Path(__file__).parent / "t1_047_variations_results.csv"
    print(f"t1_047 variations: {len(CONFIGS)} configs × {BUDGET_S:.0f}s = "
          f"{len(CONFIGS) * BUDGET_S:.0f}s max wall", flush=True)
    t_start = time.monotonic()
    for i, (name, flags) in enumerate(CONFIGS):
        print(f"  [{i+1}/{len(CONFIGS)}] {name} ...", end=" ", flush=True)
        row = run_one(name, flags)
        stat = "SOLVE" if row["finished"] else "TIMEOUT"
        pb = row["final_progress_bits"]
        pb_str = f"{pb:.3f}" if pb is not None else "?"
        pct = row["final_pct_lin"]
        pct_str = f"{pct:.3f}%" if pct is not None else "?"
        dec = row["final_decisions"]
        print(f"{stat} progress_bits={pb_str} pct_lin={pct_str} dec={dec} "
              f"wall={row['wall_s']:.2f}s", flush=True)
        append_row(out_path, row, first=(i == 0))
    elapsed = time.monotonic() - t_start
    print(f"\nDone in {elapsed:.1f}s. Results: {out_path}", flush=True)


if __name__ == "__main__":
    main()
