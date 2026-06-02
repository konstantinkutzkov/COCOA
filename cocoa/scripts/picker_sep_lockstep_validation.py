#!/usr/bin/env python3
"""
Validate -pickerSepLockstep across the instances we have baselines for.

For each instance, three configs:
  1. plain        : -rec -sep 5 -cb 3 -sepMode metis
  2. picker-lockstep:  + -unifiedPicker -decomposeAfterK 1000 -pickerSepLockstep
  3. triple+lockstep:  + -wlIter 2 -reactiveMetis ... -unifiedPicker
                       -decomposeAfterK 1000 -pickerSepLockstep

Expected:
  - (1) ≈ (2) on small instances (lockstep should mimic plain).
  - (3) preserves the t1_041 win while not regressing on small instances.

Writes results to picker_sep_lockstep_validation_results.csv.
"""
import csv
import re
import subprocess
import sys
import time
from pathlib import Path


SHARPSAT = Path("build/sharpSAT").resolve()
TEMP_CNF_DIR = Path("../temp_cnf").resolve()

# (cnf basename, expected count, per-instance budget seconds)
INSTANCES = [
    ("mc2025_track1_065.cnf",
     "37778931862957161709568", 30),
    ("mc2025_track1_071.cnf",
     "456295684783698132731653351484293780287639045166077370506304563500761788632102076272640",
     30),
    ("mc2025_track1_025.cnf", "134746112245856", 60),
    ("mc2025_track1_027.cnf", "1115259056499565", 60),
    ("mc2025_track1_041.cnf",
     "55167673345665723920748518509559191427126157713600668747551155984464814565"
     "85242921453289087159758109276082076143389903536672402142627620677878447695"
     "67285457944155885173406652398097704601520118639414720871899678052733767515"
     "08436564089844981127155486259410453216757980597598419691809480935938892302"
     "72947396172414327489978619254427105739355678825562850008814012894550076942"
     "77186576350749602616471872371428033351209908576669152300840570829254432598"
     "92051805732864",
     60),
]

CONFIGS = [
    ("plain",
     "-rec -sep 5 -cb 3 -sepMode metis"),
    ("picker+lockstep",
     "-rec -sep 5 -cb 3 -sepMode metis -unifiedPicker -decomposeAfterK 1000 -pickerSepLockstep"),
    ("triple+lockstep",
     "-rec -sep 5 -cb 3 -sepMode metis -wlIter 2 -reactiveMetis -reactiveMetisMin 10 "
     "-reactiveMetisSkip 4 -unifiedPicker -decomposeAfterK 1000 -pickerSepLockstep -cascadeW 0"),
]

COUNT_RE = re.compile(r"^\s*(\d{8,})\s*$", re.MULTILINE)
TIME_RE = re.compile(r"^\s*time:\s*([\d.]+)\s*s", re.MULTILINE)
DIAG_RE = re.compile(r"DIAG_STATS\s+num_decisions=(\d+)")


def extract_count(out: str) -> str:
    cands = COUNT_RE.findall(out)
    return max(cands, key=len) if cands else ""


FIELDNAMES = ["cnf", "config", "wall_s", "solver_time_s", "finished",
              "count_match", "decisions"]


def append_row(out_path: Path, row: dict, first: bool) -> None:
    mode = "w" if first else "a"
    with open(out_path, mode, newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDNAMES, extrasaction="ignore")
        if first:
            w.writeheader()
        w.writerow(row)


def run_one(cnf: Path, expected: str, budget: int, name: str, flags: str) -> dict:
    cmd = [str(SHARPSAT)] + flags.split() + ["-t", str(budget), str(cnf)]
    t0 = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=budget + 10)
        wall = time.monotonic() - t0
        out = (proc.stdout or "") + "\n" + (proc.stderr or "")
    except subprocess.TimeoutExpired as e:
        wall = time.monotonic() - t0
        out = ((e.stdout or b"").decode("utf-8", "replace") + "\n" +
               (e.stderr or b"").decode("utf-8", "replace"))
    count = extract_count(out)
    finished = bool(count)
    count_match = (count == expected) if finished else False
    m = TIME_RE.search(out)
    solver_t = float(m.group(1)) if m else None
    m = DIAG_RE.search(out)
    decisions = int(m.group(1)) if m else None
    return {
        "cnf": cnf.name,
        "config": name,
        "wall_s": round(wall, 3),
        "solver_time_s": solver_t,
        "finished": finished,
        "count_match": count_match,
        "decisions": decisions,
    }


def main():
    if not SHARPSAT.exists():
        sys.exit(f"sharpSAT not found: {SHARPSAT}")
    out_path = Path(__file__).parent / "picker_sep_lockstep_validation_results.csv"
    print(f"Lockstep validation: {len(INSTANCES)} instances × "
          f"{len(CONFIGS)} configs = {len(INSTANCES) * len(CONFIGS)} runs",
          flush=True)
    n_written = 0
    t_start = time.monotonic()
    for cnf_basename, expected, budget in INSTANCES:
        cnf_path = TEMP_CNF_DIR / cnf_basename
        if not cnf_path.exists():
            print(f"  SKIP {cnf_basename}: not found", flush=True)
            continue
        print(f"=== {cnf_basename} (budget {budget}s) ===", flush=True)
        for name, flags in CONFIGS:
            print(f"  {name} ...", end=" ", flush=True)
            row = run_one(cnf_path, expected, budget, name, flags)
            cm = "✓" if row["count_match"] else ("?" if row["finished"] else "—")
            stat = "SOLVE" if row["finished"] else "TIMEOUT"
            t = row["solver_time_s"]
            t_str = f"{t:.2f}s" if t is not None else "?"
            dec = row["decisions"] if row["decisions"] is not None else "?"
            print(f"{stat} count={cm} time={t_str} wall={row['wall_s']:.2f}s "
                  f"dec={dec}", flush=True)
            append_row(out_path, row, first=(n_written == 0))
            n_written += 1
    elapsed = time.monotonic() - t_start
    print(f"\nDone in {elapsed:.1f}s. Results: {out_path}", flush=True)


if __name__ == "__main__":
    main()
