#!/usr/bin/env python3
"""
Adaptive-focused variation sweep on t1_047.

Five configs, run sequentially, each with 15-minute budget and PROGRESS
emitted every minute. Captures trajectory + final state per config.

Baseline: `-adaptive` solved t1_047 in ~11-12 min in the earlier run.

Configs:
  1. adaptive_default       : -adaptive (the 2026-05-19 winner reference)
  2. adaptive_wl2           : + -wlIter 2 (reduces canonical-key collisions)
  3. adaptive_react         : + -reactiveMetis (dynamic cuts when needed)
  4. adaptive_alpha0.5      : + -adaptiveAlpha 0.5 (less length-decay)
  5. adaptive_alpha1.0      : + -adaptiveAlpha 1.0 (midway)

Writes per-config CSV row to adaptive_variations_t1_047_results.csv
incrementally so partial results survive interruption.
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
BUDGET_S = 900.0  # 15 min per config
PROGRESS_INTERVAL_S = 60.0  # 1 min between PROGRESS ticks

CONFIGS = [
    ("adaptive_default",
     "-rec -sep 5 -cb 3 -sepMode metis -adaptive"),
    ("adaptive_wl2",
     "-rec -sep 5 -cb 3 -sepMode metis -adaptive -wlIter 2"),
    ("adaptive_react",
     "-rec -sep 5 -cb 3 -sepMode metis -adaptive "
     "-reactiveMetis -reactiveMetisMin 10 -reactiveMetisSkip 4"),
    ("adaptive_alpha0.5",
     "-rec -sep 5 -cb 3 -sepMode metis -adaptive -adaptiveAlpha 0.5"),
    ("adaptive_alpha1.0",
     "-rec -sep 5 -cb 3 -sepMode metis -adaptive -adaptiveAlpha 1.0"),
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

FIELDNAMES = ["config", "finished", "wall_s", "solver_time_s",
              "final_progress_bits", "final_pct_lin", "final_decisions",
              "final_l2_hits", "n_open_comps", "trajectory_json"]


def append_row(out_path: Path, row: dict, first: bool) -> None:
    mode = "w" if first else "a"
    with open(out_path, mode, newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDNAMES, extrasaction="ignore")
        if first:
            w.writeheader()
        w.writerow(row)


def extract_count(out: str) -> str:
    cands = COUNT_RE.findall(out)
    return max(cands, key=len) if cands else ""


def run_one(name: str, flags: str) -> dict:
    env = os.environ.copy()
    env["SHARPSAT_PROGRESS"] = "1"
    env["SHARPSAT_PROGRESS_INTERVAL"] = str(PROGRESS_INTERVAL_S)
    cmd = ([str(SHARPSAT)] + flags.split() +
           ["-t", f"{BUDGET_S:.1f}", str(TEMP_CNF_DIR / CNF)])
    # Stream stderr as it arrives so PROGRESS lines flow through the
    # outer Monitor pipeline in real time.
    print(f"=== {name} ===", flush=True)
    print(f"    flags: {flags}", flush=True)
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=env, text=True, bufsize=1)
    stderr_lines = []
    stdout_lines = []
    # Read stderr line-by-line for live progress; read stdout at end for count.
    try:
        for line in proc.stderr:
            stderr_lines.append(line)
            stripped = line.rstrip()
            if stripped.startswith("PROGRESS") or stripped.startswith("OPEN_WORK"):
                print(f"    {stripped}", flush=True)
        proc.wait(timeout=BUDGET_S + 30.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    # Drain remaining stdout (count + diagnostics).
    if proc.stdout:
        stdout_lines = proc.stdout.read().splitlines()
    wall = time.monotonic() - t0
    out = "\n".join(stdout_lines) + "\n" + "".join(stderr_lines)
    count = extract_count(out)
    finished = bool(count)
    m = TIME_RE.search(out)
    solver_t = float(m.group(1)) if m else None
    # Build trajectory from stderr.
    traj = []
    for m in PROGRESS_RE.finditer("".join(stderr_lines)):
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
    m = OPEN_RE.search("".join(stderr_lines))
    n_open_comps = int(m.group(2)) if m else None
    final_pb = float(m.group(4)) if m else None
    final_pct_lin = traj[-1]["pct_lin"] if traj else None
    final_dec = traj[-1]["decisions"] if traj else None
    final_hits = traj[-1]["l2_hits"] if traj else None
    stat = "SOLVE" if finished else "TIMEOUT"
    print(f"    {stat} wall={wall:.1f}s solver={solver_t}s "
          f"final_progress_bits={final_pb}", flush=True)
    return {
        "config": name,
        "finished": finished,
        "wall_s": round(wall, 3),
        "solver_time_s": solver_t,
        "final_progress_bits": final_pb,
        "final_pct_lin": final_pct_lin,
        "final_decisions": final_dec,
        "final_l2_hits": final_hits,
        "n_open_comps": n_open_comps,
        "trajectory_json": json.dumps(traj),
    }


def main():
    if not SHARPSAT.exists():
        sys.exit(f"sharpSAT not found: {SHARPSAT}")
    out_path = Path(__file__).parent / "adaptive_variations_t1_047_results.csv"
    print(f"adaptive variations on {CNF}: {len(CONFIGS)} configs × "
          f"{BUDGET_S:.0f}s = {len(CONFIGS) * BUDGET_S / 60:.0f} min max wall",
          flush=True)
    t_start = time.monotonic()
    for i, (name, flags) in enumerate(CONFIGS):
        row = run_one(name, flags)
        append_row(out_path, row, first=(i == 0))
    elapsed = time.monotonic() - t_start
    print(f"\nAll configs done in {elapsed:.1f}s "
          f"({elapsed/60:.1f} min). Results: {out_path}", flush=True)


if __name__ == "__main__":
    main()
