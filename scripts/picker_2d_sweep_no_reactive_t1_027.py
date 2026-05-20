#!/usr/bin/env python3
"""
2D picker-parameter sweep on t1_027 *without* reactiveMetis.

Same as picker_2d_sweep_t1_027.py but drops -reactiveMetis* from the base.
Isolates picker contribution from reactive-METIS contribution.

Axes:
  -wlIter:        {1, 2}
  -pickerAlphaVar:{15 (default), 50, 100, 200}

Base flags held fixed:
    -rec -sep 5 -cb 3 -sepMode metis
    -unifiedPicker -decomposeAfterK 1000 -cascadeW 0
    (NO -reactiveMetis)

Budget: 60s per run.

Writes results to picker_2d_sweep_no_reactive_t1_027_results.csv (incremental).
"""
import csv
import re
import subprocess
import sys
import time
from pathlib import Path


SHARPSAT = Path("build/sharpSAT").resolve()
TEMP_CNF_DIR = Path("../temp_cnf").resolve()

CNF = "mc2025_track1_027.cnf"
EXPECTED_COUNT = "1115259056499565"
BUDGET_S = 60.0

BASE_FLAGS = (
    "-rec -sep 5 -cb 3 -sepMode metis "
    "-unifiedPicker -decomposeAfterK 1000 -cascadeW 0"
)

WL_VALUES = [1, 2]
ALPHA_VAR_VALUES = [15, 50, 100, 200]

COUNT_RE = re.compile(r"^\s*(\d{8,})\s*$", re.MULTILINE)
TIME_RE = re.compile(r"^\s*time:\s*([\d.]+)\s*s", re.MULTILINE)
OPEN_RE = re.compile(
    r"OPEN_WORK\s+n_root=(\d+)\s+n_open_comps=(\d+)\s+"
    r"bound_log2=([\d.eE+-]+)\s+progress_bits=([\d.eE+-]+)"
)
DIAG_RE = re.compile(r"DIAG_STATS\s+num_decisions=(\d+)")
FULL_RE = re.compile(r"FULL_CACHE_STATS\s+l2_stores=(\d+)\s+l2_hits=(\d+)")


def extract_count(out: str) -> str:
    cands = COUNT_RE.findall(out)
    return max(cands, key=len) if cands else ""


FIELDNAMES = ["wlIter", "alpha_var", "finished", "count_match",
              "wall_s", "solver_time_s", "progress_bits",
              "n_open_comps", "bound_log2",
              "decisions", "l2_hits", "count"]


def append_row(out_path: Path, row: dict, first: bool) -> None:
    mode = "w" if first else "a"
    with open(out_path, mode, newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDNAMES, extrasaction="ignore")
        if first:
            w.writeheader()
        w.writerow(row)


def run_one(wl: int, av: int) -> dict:
    cmd = ([str(SHARPSAT)] + BASE_FLAGS.split() +
           ["-wlIter", str(wl),
            "-pickerAlphaVar", str(av),
            "-t", f"{BUDGET_S:.1f}",
            str(TEMP_CNF_DIR / CNF)])
    t0 = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=BUDGET_S + 10.0)
        wall = time.monotonic() - t0
        out = (proc.stdout or "") + "\n" + (proc.stderr or "")
    except subprocess.TimeoutExpired as e:
        wall = time.monotonic() - t0
        out = ((e.stdout or b"").decode("utf-8", "replace") + "\n" +
               (e.stderr or b"").decode("utf-8", "replace"))

    count = extract_count(out)
    finished = bool(count)
    count_match = (count == EXPECTED_COUNT) if finished else False
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
    return {
        "wlIter": wl,
        "alpha_var": av,
        "finished": finished,
        "count_match": count_match,
        "wall_s": round(wall, 3),
        "solver_time_s": solver_time_s,
        "progress_bits": progress_bits,
        "n_open_comps": n_open_comps,
        "bound_log2": bound_log2,
        "decisions": decisions,
        "l2_hits": l2_hits,
        "count": count if not count_match else "(matches)",
    }


def main():
    if not SHARPSAT.exists():
        sys.exit(f"sharpSAT not found: {SHARPSAT}")
    out_path = Path(__file__).parent / "picker_2d_sweep_no_reactive_t1_027_results.csv"
    runs = [(wl, av) for wl in WL_VALUES for av in ALPHA_VAR_VALUES]
    print(f"2D picker sweep (no reactiveMetis) on {CNF}: "
          f"{len(runs)} runs, budget={BUDGET_S:.0f}s each", flush=True)
    print(f"Base: {BASE_FLAGS}", flush=True)
    print(f"Expected count: {EXPECTED_COUNT}\n", flush=True)
    t_start = time.monotonic()
    for i, (wl, av) in enumerate(runs):
        print(f"  [{i+1}/{len(runs)}] wlIter={wl} alpha_var={av} ...",
              end=" ", flush=True)
        row = run_one(wl, av)
        cm = "✓" if row["count_match"] else ("?" if row["finished"] else "—")
        pb = row["progress_bits"]
        pb_str = f"{pb:.2f}" if pb is not None else "?"
        dec = row["decisions"] if row["decisions"] is not None else "?"
        print(f"{'SOLVE' if row['finished'] else 'TIMEOUT'} count={cm} "
              f"wall={row['wall_s']:.2f}s progress_bits={pb_str} dec={dec}",
              flush=True)
        append_row(out_path, row, first=(i == 0))
    elapsed = time.monotonic() - t_start
    print(f"\nDone in {elapsed:.1f}s. Results: {out_path}", flush=True)


if __name__ == "__main__":
    main()
