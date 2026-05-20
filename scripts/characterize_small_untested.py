#!/usr/bin/env python3
"""
Quick characterization sweep on untested small instances.
For each instance, run a few representative configs at 60s budget.
"""
import csv
import re
import subprocess
import sys
import time
from pathlib import Path


SHARPSAT = Path("build/sharpSAT").resolve()
TEMP_CNF_DIR = Path("../temp_cnf").resolve()
BUDGET_S = 60.0

INSTANCES = [
    "mc2025_track1_045.cnf",   # 90v 270c d=3.0
    "mc2025_track1_163.cnf",   # 256v 1024c d=4.0
    "mc2025_track1_159.cnf",   # 256v 1038c
    "mc2025_track1_053.cnf",   # 222v 1186c d=5.3
    "mc2025_track1_003.cnf",   # 264v 1474c d=5.6
    "mc2025_track1_059.cnf",   # 264v 1552c d=5.9
    "mc2025_track1_005.cnf",   # 378v 1893c
    "mc2025_track1_105.cnf",   # 729v 3172c
]

CONFIGS = [
    ("plain",
     "-rec -sep 5 -cb 3 -sepMode metis"),
    ("adaptive",
     "-rec -sep 5 -cb 3 -sepMode metis -adaptive"),
    ("triple+lockstep",
     "-rec -sep 5 -cb 3 -sepMode metis -wlIter 2 -reactiveMetis "
     "-reactiveMetisMin 10 -reactiveMetisSkip 4 -unifiedPicker "
     "-decomposeAfterK 1000 -pickerSepLockstep -cascadeW 0"),
]

COUNT_RE = re.compile(r"^\s*(\d{8,})\s*$", re.MULTILINE)
TIME_RE = re.compile(r"^\s*time:\s*([\d.]+)\s*s", re.MULTILINE)
OPEN_RE = re.compile(
    r"OPEN_WORK\s+n_root=(\d+)\s+n_open_comps=(\d+)\s+"
    r"bound_log2=([\d.eE+-]+)\s+progress_bits=([\d.eE+-]+)"
)


def extract_count(out: str) -> str:
    cands = COUNT_RE.findall(out)
    return max(cands, key=len) if cands else ""


FIELDNAMES = ["cnf", "config", "wall_s", "solver_time_s", "finished",
              "count", "n_root", "n_open_comps", "progress_bits",
              "pct_lin_approx"]


def append_row(out_path: Path, row: dict, first: bool) -> None:
    mode = "w" if first else "a"
    with open(out_path, mode, newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDNAMES, extrasaction="ignore")
        if first:
            w.writeheader()
        w.writerow(row)


def run_one(cnf: Path, name: str, flags: str) -> dict:
    cmd = [str(SHARPSAT)] + flags.split() + ["-t", str(BUDGET_S), str(cnf)]
    t0 = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=BUDGET_S + 10)
        wall = time.monotonic() - t0
        out = (proc.stdout or "") + "\n" + (proc.stderr or "")
    except subprocess.TimeoutExpired as e:
        wall = time.monotonic() - t0
        out = ((e.stdout or b"").decode("utf-8", "replace") + "\n" +
               (e.stderr or b"").decode("utf-8", "replace"))
    count = extract_count(out)
    finished = bool(count)
    m = TIME_RE.search(out)
    solver_t = float(m.group(1)) if m else None
    m = OPEN_RE.search(out)
    n_root = int(m.group(1)) if m else None
    n_open_comps = int(m.group(2)) if m else None
    progress_bits = float(m.group(4)) if m else None
    pct_lin = None
    if n_root is not None and progress_bits is not None:
        if progress_bits >= n_root:
            pct_lin = 100.0
        elif progress_bits > 60:
            pct_lin = 100.0
        else:
            pct_lin = (1.0 - 2.0 ** (-progress_bits)) * 100.0
    return {
        "cnf": cnf.name,
        "config": name,
        "wall_s": round(wall, 3),
        "solver_time_s": solver_t,
        "finished": finished,
        "count": count if finished else "",
        "n_root": n_root,
        "n_open_comps": n_open_comps,
        "progress_bits": progress_bits,
        "pct_lin_approx": pct_lin,
    }


def main():
    if not SHARPSAT.exists():
        sys.exit(f"sharpSAT not found: {SHARPSAT}")
    out_path = Path(__file__).parent / "characterize_small_untested_results.csv"
    n_runs = sum(1 for c in INSTANCES if (TEMP_CNF_DIR / c).exists()) * len(CONFIGS)
    print(f"Characterization sweep: {len(INSTANCES)} instances × "
          f"{len(CONFIGS)} configs = {n_runs} runs, {BUDGET_S:.0f}s each",
          flush=True)
    n_written = 0
    t_start = time.monotonic()
    for cnf_basename in INSTANCES:
        cnf_path = TEMP_CNF_DIR / cnf_basename
        if not cnf_path.exists():
            print(f"  SKIP {cnf_basename}: not found", flush=True)
            continue
        print(f"=== {cnf_basename} ===", flush=True)
        for name, flags in CONFIGS:
            print(f"  {name} ...", end=" ", flush=True)
            row = run_one(cnf_path, name, flags)
            stat = "SOLVE" if row["finished"] else "TIMEOUT"
            pct = row["pct_lin_approx"]
            pct_str = f"{pct:.3g}%" if pct is not None else "?"
            t = row["solver_time_s"]
            t_str = f"{t:.2f}s" if t is not None else f"{row['wall_s']:.2f}s"
            print(f"{stat} time={t_str} progress~{pct_str} "
                  f"n_root={row['n_root']}", flush=True)
            append_row(out_path, row, first=(n_written == 0))
            n_written += 1
    elapsed = time.monotonic() - t_start
    print(f"\nDone in {elapsed:.1f}s. Results: {out_path}", flush=True)


if __name__ == "__main__":
    main()
