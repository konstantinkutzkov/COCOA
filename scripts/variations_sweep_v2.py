#!/usr/bin/env python3
"""
12-config portfolio sweep v2 — refined post-optimization-stack 2026-05-21.

Adds adaptive+wl2 (which solved t1_045 first time, 40.5 min).
Drops 'plain' (kept per [[plain-baseline-required]]); drops clausesFirst,
picker_vanilla, and one of pickerRootSepOnly/pickerNonSepKillsNd (kept
rootSepOnly as representative of "deeper subs lose ND" since
nonSepKillsNd has a more conditional trigger).

Usage:
    python3 scripts/variations_sweep_v2.py <cnf_basename> [budget_seconds]

Default budget: 60s per config. Output CSV:
    variations_v2_<cnf_basename_stem>_results.csv
"""
import csv
import os
import re
import subprocess
import sys
import time
from pathlib import Path


SHARPSAT = Path("build/sharpSAT").resolve()
TEMP_CNF_DIR = Path("../temp_cnf").resolve()
PROGRESS_INTERVAL_S = 5.0

CONFIGS = [
    # No-picker family (6)
    ("plain",
     "-rec -sep 5 -cb 3 -sepMode metis"),
    ("plain+react",
     "-rec -sep 5 -cb 3 -sepMode metis -reactiveMetis -reactiveMetisMin 10 "
     "-reactiveMetisSkip 4"),
    ("plain+wl2+react",
     "-rec -sep 5 -cb 3 -sepMode metis -wlIter 2 -reactiveMetis -reactiveMetisMin 10 "
     "-reactiveMetisSkip 4"),
    ("legacy_sepVarBias",
     "-rec -sep 5 -cb 3 -sepMode metis -sepVarBias"),
    ("adaptive",
     "-rec -sep 5 -cb 3 -sepMode metis -adaptive"),
    ("adaptive+wl2",
     "-rec -sep 5 -cb 3 -sepMode metis -adaptive -wlIter 2"),
    # Picker family (6)
    ("picker_alpha100",
     "-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000 "
     "-pickerAlphaVar 100 -pickerAlphaClause 100"),
    ("picker_rootSepOnly",
     "-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000 "
     "-pickerRootSepOnly"),
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


def extract_count(out: str) -> str:
    cands = COUNT_RE.findall(out)
    return max(cands, key=len) if cands else ""


FIELDNAMES = ["config", "finished", "wall_s", "solver_time_s",
              "final_progress_bits", "final_pct_lin", "final_closed_bits",
              "final_decisions", "final_l2_hits",
              "n_root", "n_open_comps", "count"]


def append_row(out_path: Path, row: dict, first: bool) -> None:
    mode = "w" if first else "a"
    with open(out_path, mode, newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDNAMES, extrasaction="ignore")
        if first:
            w.writeheader()
        w.writerow(row)


def run_one(cnf: Path, name: str, flags: str, budget_s: float) -> dict:
    env = os.environ.copy()
    env["SHARPSAT_PROGRESS"] = "1"
    env["SHARPSAT_PROGRESS_INTERVAL"] = str(PROGRESS_INTERVAL_S)
    cmd = [str(SHARPSAT)] + flags.split() + ["-t", f"{budget_s:.1f}", str(cnf)]
    t0 = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=budget_s + 10.0, env=env)
        wall = time.monotonic() - t0
        out = (proc.stdout or "") + "\n" + (proc.stderr or "")
        stderr = proc.stderr or ""
    except subprocess.TimeoutExpired as e:
        wall = time.monotonic() - t0
        out = ((e.stdout or b"").decode("utf-8", "replace") + "\n" +
               (e.stderr or b"").decode("utf-8", "replace"))
        stderr = (e.stderr or b"").decode("utf-8", "replace")
    count = extract_count(out)
    finished = bool(count)
    m = TIME_RE.search(out)
    solver_t = float(m.group(1)) if m else None
    m = OPEN_RE.search(stderr)
    n_root = int(m.group(1)) if m else None
    n_open_comps = int(m.group(2)) if m else None
    final_pb = float(m.group(4)) if m else None
    final_pct_lin = final_closed = final_dec = final_hits = None
    for m in PROGRESS_RE.finditer(stderr):
        final_pct_lin = float(m.group(2))
        final_closed = float(m.group(4))
        final_dec = int(m.group(7))
        final_hits = int(m.group(8))
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
        "count": count if finished else "",
    }


def main():
    if len(sys.argv) < 2:
        sys.exit("Usage: variations_sweep_v2.py <cnf_basename> [budget_seconds]")
    cnf_basename = sys.argv[1]
    budget_s = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    if not SHARPSAT.exists():
        sys.exit(f"sharpSAT not found: {SHARPSAT}")
    cnf_path = TEMP_CNF_DIR / cnf_basename
    if not cnf_path.exists():
        sys.exit(f"CNF not found: {cnf_path}")
    stem = cnf_basename.replace(".cnf", "")
    out_path = Path(__file__).parent / f"variations_v2_{stem}_results.csv"
    print(f"{cnf_basename}: {len(CONFIGS)} configs × {budget_s:.0f}s each "
          f"(max {len(CONFIGS) * budget_s:.0f}s wall)", flush=True)
    print(f"Output → {out_path}\n", flush=True)
    t_start = time.monotonic()
    for i, (name, flags) in enumerate(CONFIGS):
        print(f"  [{i+1}/{len(CONFIGS)}] {name} ...", end=" ", flush=True)
        row = run_one(cnf_path, name, flags, budget_s)
        stat = "SOLVE" if row["finished"] else "TIMEOUT"
        pb = row["final_progress_bits"]
        pb_str = f"{pb:.4g}" if pb is not None else "?"
        pct = row["final_pct_lin"]
        pct_str = f"{pct:.4g}%" if pct is not None else "?"
        cb = row["final_closed_bits"]
        cb_str = f"{cb:.2f}" if cb is not None else "?"
        t = row["solver_time_s"]
        t_str = f"{t:.2f}s" if t is not None else f"{row['wall_s']:.2f}s"
        print(f"{stat} pb={pb_str} pct_lin={pct_str} cb={cb_str} t={t_str}",
              flush=True)
        append_row(out_path, row, first=(i == 0))
    elapsed = time.monotonic() - t_start
    print(f"\nDone in {elapsed:.1f}s. Results: {out_path}", flush=True)


if __name__ == "__main__":
    main()
