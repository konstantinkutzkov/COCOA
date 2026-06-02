#!/usr/bin/env python3
"""
Density-1-structured class probe: does today's triple-flag config (sharpSAT
with -wlIter 2 -reactiveMetis* -unifiedPicker -decomposeAfterK 1000 -cascadeW 0)
solve the four density-1 structured instances flagged 2026-05-11 (t1_023,
t1_025, t1_027, t1_047) that previously timed out on legacy/plain/ganak at 60s?

Per instance, runs (with a 600s budget per run):
  1. ganak --verb 0 (default --td 1)
  2. ganak --verb 0 --td 0     [only if (1) timed out]
  3. sharpSAT triple-flag config

Cross-checks the model count between sharpSAT and whichever ganak run produced
one. For instances with documented counts (t1_025, t1_027) we also verify
against those.

Writes density1_class_probe_results.csv with all per-run data.

Run from sharpsat-separator/ directory:
    python3 scripts/density1_class_probe.py
"""
import csv
import re
import subprocess
import sys
import time
from pathlib import Path


SHARPSAT = Path("build/sharpSAT").resolve()
GANAK = Path("../ganak/build/ganak").resolve()
TEMP_CNF_DIR = Path("../temp_cnf").resolve()

BUDGET_S = 600.0

SHARPSAT_FLAGS = (
    "-rec -sep 5 -cb 3 -sepMode metis -wlIter 2 "
    "-reactiveMetis -reactiveMetisMin 10 -reactiveMetisSkip 4 "
    "-unifiedPicker -decomposeAfterK 1000 -cascadeW 0"
)

# (cnf basename, documented_count_or_None)
INSTANCES = [
    ("mc2025_track1_023.cnf", None),
    ("mc2025_track1_025.cnf", "134746112245856"),
    ("mc2025_track1_027.cnf", "1115259056499565"),
    ("mc2025_track1_047.cnf", None),
]

COUNT_RE = re.compile(r"^\s*(\d{8,})\s*$", re.MULTILINE)
GANAK_COUNT_RE = re.compile(r"c s exact arb int\s+(\d+)")
GANAK_TIME_RE = re.compile(r"Total time\s+:\s*([\d.]+)\s*s", re.IGNORECASE)
SHARPSAT_TIME_RE = re.compile(r"^\s*time:\s*([\d.]+)\s*s", re.MULTILINE)
OPEN_RE = re.compile(
    r"OPEN_WORK\s+n_root=(\d+)\s+n_open_comps=(\d+)\s+"
    r"bound_log2=([\d.eE+-]+)\s+progress_bits=([\d.eE+-]+)"
)


def extract_count_generic(out: str) -> str:
    """Return the largest big-integer found anywhere in the output."""
    # Prefer ganak's explicit line if present.
    m = GANAK_COUNT_RE.search(out)
    if m:
        return m.group(1)
    cands = COUNT_RE.findall(out)
    return max(cands, key=len) if cands else ""


def run_solver(cmd: list, budget_s: float) -> tuple:
    """Run a solver command. Returns (out, wall_seconds, finished)."""
    t0 = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=budget_s + 10.0)
        wall = time.monotonic() - t0
        out = (proc.stdout or "") + "\n" + (proc.stderr or "")
        # Heuristic: finished iff there's a count in the output.
        finished = bool(extract_count_generic(out))
        return out, wall, finished
    except subprocess.TimeoutExpired as e:
        wall = time.monotonic() - t0
        out = ((e.stdout or b"").decode("utf-8", "replace") + "\n" +
               (e.stderr or b"").decode("utf-8", "replace"))
        return out, wall, False


def run_ganak(cnf_path: Path, td_mode: int) -> dict:
    cmd = [str(GANAK), "--verb", "0", "--td", str(td_mode), str(cnf_path)]
    out, wall, _ = run_solver(cmd, BUDGET_S)
    count = extract_count_generic(out)
    return {
        "solver": f"ganak_td{td_mode}",
        "count": count,
        "wall_s": round(wall, 3),
        "finished": bool(count),
    }


def run_sharpsat(cnf_path: Path) -> dict:
    cmd = ([str(SHARPSAT)] + SHARPSAT_FLAGS.split() +
           ["-t", f"{BUDGET_S:.1f}", str(cnf_path)])
    out, wall, _ = run_solver(cmd, BUDGET_S)
    count = extract_count_generic(out)
    m = OPEN_RE.search(out)
    if m:
        n_open_comps = int(m.group(2))
        bound_log2 = float(m.group(3))
        progress_bits = float(m.group(4))
        n_root = int(m.group(1))
    else:
        n_open_comps = bound_log2 = progress_bits = n_root = None
    return {
        "solver": "sharpsat_triple",
        "count": count,
        "wall_s": round(wall, 3),
        "finished": bool(count),
        "n_root": n_root,
        "n_open_comps": n_open_comps,
        "bound_log2": bound_log2,
        "progress_bits": progress_bits,
    }


FIELDNAMES = ["cnf", "solver", "finished", "wall_s", "count",
              "count_match", "documented_count", "n_root",
              "n_open_comps", "bound_log2", "progress_bits"]


def append_row(out_path: Path, row: dict, first: bool) -> None:
    """Append one row to CSV. Writes header on first call."""
    mode = "w" if first else "a"
    with open(out_path, mode, newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDNAMES, extrasaction="ignore")
        if first:
            w.writeheader()
        w.writerow(row)


def main():
    if not SHARPSAT.exists():
        sys.exit(f"sharpSAT not found: {SHARPSAT}")
    if not GANAK.exists():
        sys.exit(f"ganak not found: {GANAK}")

    out_path = Path(__file__).parent / "density1_class_probe_results.csv"
    print(f"density-1 class probe: 4 instances, budget={BUDGET_S}s/run.",
          flush=True)
    print(f"sharpSAT base: {SHARPSAT_FLAGS}\n", flush=True)

    t_start = time.monotonic()
    n_written = 0
    for cnf_basename, documented_count in INSTANCES:
        cnf_path = TEMP_CNF_DIR / cnf_basename
        if not cnf_path.exists():
            print(f"  SKIP {cnf_basename}: not found", flush=True)
            continue
        print(f"=== {cnf_basename} ===", flush=True)

        # 1. ganak td=1
        print(f"  ganak --td 1 (budget {BUDGET_S:.0f}s) ...",
              end=" ", flush=True)
        g1 = run_ganak(cnf_path, td_mode=1)
        print(f"{'SOLVE' if g1['finished'] else 'TIMEOUT'} "
              f"wall={g1['wall_s']:.2f}s", flush=True)
        row = {**g1, "cnf": cnf_basename,
               "documented_count": documented_count}
        append_row(out_path, row, first=(n_written == 0))
        n_written += 1

        # 2. ganak td=0 only if td=1 timed out
        if not g1["finished"]:
            print(f"  ganak --td 0 (budget {BUDGET_S:.0f}s) ...",
                  end=" ", flush=True)
            g0 = run_ganak(cnf_path, td_mode=0)
            print(f"{'SOLVE' if g0['finished'] else 'TIMEOUT'} "
                  f"wall={g0['wall_s']:.2f}s", flush=True)
            row = {**g0, "cnf": cnf_basename,
                   "documented_count": documented_count}
            append_row(out_path, row, first=False)
            n_written += 1
            ganak_count = g0.get("count", "")
        else:
            ganak_count = g1.get("count", "")

        # 3. sharpSAT triple-flag config
        print(f"  sharpSAT triple-flag (budget {BUDGET_S:.0f}s) ...",
              end=" ", flush=True)
        s = run_sharpsat(cnf_path)
        # Reference count: documented if known, else whichever ganak run
        # produced a count.
        ref = documented_count or ganak_count
        if s["finished"] and ref:
            cm = "match" if s["count"] == ref else "MISMATCH"
        elif s["finished"]:
            cm = "no-ref"
        else:
            cm = "n/a"
        pb = s["progress_bits"]
        pb_str = f"{pb:.2f}" if pb is not None else "?"
        print(f"{'SOLVE' if s['finished'] else 'TIMEOUT'} count={cm} "
              f"wall={s['wall_s']:.2f}s progress_bits={pb_str}", flush=True)
        row = {**s, "cnf": cnf_basename,
               "documented_count": documented_count,
               "count_match": cm}
        append_row(out_path, row, first=False)
        n_written += 1

    elapsed = time.monotonic() - t_start
    print(f"\nDone in {elapsed:.1f}s ({elapsed/60:.1f} min). "
          f"Results: {out_path}", flush=True)


if __name__ == "__main__":
    main()
