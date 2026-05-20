#!/usr/bin/env python3
"""
Single-axis sweep: -cascadeW on react-agg-wl2 + unifiedPicker base.

Base config (held fixed):
    -rec -sep 5 -cb 3 -sepMode metis -wlIter 2
    -reactiveMetis -reactiveMetisMin 10 -reactiveMetisSkip 4
    -unifiedPicker -decomposeAfterK 1000

Sweep -cascadeW over {0, 1, 2, 5} on 4 instances. Per-instance timeout is
6*(t_best + 1) where t_best is the best documented wall time in
benchmark_log.md.

Writes results to picker_cascade_sweep_results.csv.

Run from sharpsat-separator/ directory:
    python3 scripts/picker_cascade_sweep.py
"""
import csv
import re
import subprocess
import sys
import time
from pathlib import Path


SOLVER = Path("build/sharpSAT").resolve()
TEMP_CNF_DIR = Path("../temp_cnf").resolve()

BASE_FLAGS = (
    "-rec -sep 5 -cb 3 -sepMode metis -wlIter 2 "
    "-reactiveMetis -reactiveMetisMin 10 -reactiveMetisSkip 4 "
    "-unifiedPicker -decomposeAfterK 1000"
)

# (cnf basename, expected count, best documented wall time in seconds)
INSTANCES = [
    ("mc2025_track1_065.cnf",
     "37778931862957161709568",
     0.017),
    ("mc2025_track1_071.cnf",
     "456295684783698132731653351484293780287639045166077370506304563500761788632102076272640",
     0.41),
    ("mc2025_track1_041.cnf",
     # Full count from a verified run earlier this session (matches ganak).
     "55167673345665723920748518509559191427126157713600668747551155984464814565"
     "85242921453289087159758109276082076143389903536672402142627620677878447695"
     "67285457944155885173406652398097704601520118639414720871899678052733767515"
     "08436564089844981127155486259410453216757980597598419691809480935938892302"
     "72947396172414327489978619254427105739355678825562850008814012894550076942"
     "77186576350749602616471872371428033351209908576669152300840570829254432598"
     "92051805732864",
     3.04),
    ("mc2025_track1_011.cnf",
     "536870912306",
     13.57),
]

CASCADE_VALUES = [0, 1, 2, 5]

# Match the model count on its own line in sharpSAT output.
# sharpSAT prints the count as a single big integer on a line by itself.
COUNT_RE = re.compile(r"^\s*(\d{8,})\s*$", re.MULTILINE)
TIME_RE = re.compile(r"^\s*time:\s*([\d.]+)\s*s", re.MULTILINE)
OPEN_RE = re.compile(
    r"OPEN_WORK\s+n_root=(\d+)\s+n_open_comps=(\d+)\s+"
    r"bound_log2=([\d.eE+-]+)\s+progress_bits=([\d.eE+-]+)\s+sizes=([0-9,]*)"
)
DIAG_RE = re.compile(r"DIAG_STATS\s+num_decisions=(\d+)")
FULL_RE = re.compile(r"FULL_CACHE_STATS\s+l2_stores=(\d+)\s+l2_hits=(\d+)")


def extract_count(out: str) -> str:
    """Return the largest integer found in the output (the model count)."""
    candidates = COUNT_RE.findall(out)
    if not candidates:
        return ""
    # Pick the longest (the count is the biggest number in the output).
    return max(candidates, key=len)


def run_one(cnf_path: Path, expected: str, t_best: float,
            cascade_w: int) -> dict:
    timeout_budget = 6 * (t_best + 1)
    cmd = ([str(SOLVER)] + BASE_FLAGS.split() +
           ["-cascadeW", str(cascade_w),
            "-t", f"{timeout_budget:.2f}",
            str(cnf_path)])
    t0 = time.monotonic()
    try:
        # Wall fence: budget + 5s slack in case sharpSAT misses the bound.
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout_budget + 5.0)
        wall = time.monotonic() - t0
        out = (proc.stdout or "") + "\n" + (proc.stderr or "")
        rc = proc.returncode
    except subprocess.TimeoutExpired as e:
        wall = time.monotonic() - t0
        out = (e.stdout or b"").decode("utf-8", "replace") + "\n" + \
              (e.stderr or b"").decode("utf-8", "replace")
        rc = -1

    count = extract_count(out)
    count_match = (count == expected) if count else False

    m = TIME_RE.search(out)
    solver_time_s = float(m.group(1)) if m else None

    m = OPEN_RE.search(out)
    if m:
        n_open_comps = int(m.group(2))
        bound_log2 = float(m.group(3))
        progress_bits = float(m.group(4))
    else:
        n_open_comps = bound_log2 = progress_bits = None

    m = DIAG_RE.search(out)
    decisions = int(m.group(1)) if m else None
    m = FULL_RE.search(out)
    l2_hits = int(m.group(2)) if m else None

    # "finished" means the solver returned a count, not a timeout marker.
    finished = bool(count)

    return {
        "cnf": cnf_path.name,
        "cascade_w": cascade_w,
        "timeout_budget_s": round(timeout_budget, 2),
        "wall_s": round(wall, 3),
        "solver_time_s": solver_time_s,
        "finished": finished,
        "count_match": count_match,
        "count": count if not count_match else "(matches)",
        "progress_bits": progress_bits,
        "n_open_comps": n_open_comps,
        "bound_log2": bound_log2,
        "decisions": decisions,
        "l2_hits": l2_hits,
        "rc": rc,
    }


def main():
    if not SOLVER.exists():
        sys.exit(f"Solver not found: {SOLVER}. Build first.")

    fieldnames = ["cnf", "cascade_w", "timeout_budget_s", "wall_s",
                  "solver_time_s", "finished", "count_match", "count",
                  "progress_bits", "n_open_comps", "bound_log2",
                  "decisions", "l2_hits", "rc"]

    out_path = Path(__file__).parent / "picker_cascade_sweep_results.csv"
    n_runs = len(INSTANCES) * len(CASCADE_VALUES)
    print(f"Sweep: {len(INSTANCES)} instances × {len(CASCADE_VALUES)} "
          f"cascadeW values = {n_runs} runs. Output → {out_path}")
    print(f"Solver: {SOLVER}")
    print(f"Base flags: {BASE_FLAGS}\n")

    rows = []
    t_start = time.monotonic()
    for cnf_basename, expected, t_best in INSTANCES:
        cnf_path = TEMP_CNF_DIR / cnf_basename
        if not cnf_path.exists():
            print(f"  SKIP {cnf_basename}: not found at {cnf_path}")
            continue
        for cw in CASCADE_VALUES:
            print(f"  [{len(rows)+1}/{n_runs}] {cnf_basename} cascadeW={cw} "
                  f"(timeout {6*(t_best+1):.1f}s) ...", end=" ", flush=True)
            row = run_one(cnf_path, expected, t_best, cw)
            rows.append(row)
            status = ("SOLVE" if row["finished"] else "TIMEOUT")
            cm = "✓" if row["count_match"] else (
                "✗" if row["finished"] else "—")
            pb = row["progress_bits"]
            pb_str = f"{pb:.2f}" if pb is not None else "?"
            print(f"{status} count={cm} wall={row['wall_s']:.2f}s "
                  f"progress_bits={pb_str}")

    with open(out_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        for r in rows:
            w.writerow(r)

    elapsed = time.monotonic() - t_start
    print(f"\nDone in {elapsed:.1f}s. Results: {out_path}")


if __name__ == "__main__":
    main()
