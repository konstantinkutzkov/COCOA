#!/usr/bin/env python3
"""
calibration_run.py — run a fixed manifest of (instance, flags) cases,
verify counts match expected, verify wall times haven't regressed.

Discipline: this is the harness for safe refactoring. Run before AND
after every cleanup change. If counts diverge or wall time regresses
beyond tolerance, do not commit — revert and diagnose.

Usage:
  ./calibration_run.py                    # verify against expectations
  ./calibration_run.py --solver path/to/sharpSAT
  ./calibration_run.py --bootstrap        # record actuals (use to seed
                                           # the manifest after a known-
                                           # good build); prints values
                                           # for manual entry.

Tolerance:
  - Counts must match EXACTLY (arbitrary-precision integers compared
    as strings).
  - Wall time must be within max(1 sec, 20%) of expected.

Exit code:
  0 = all pass
  1 = any fail (count mismatch OR wall regression beyond tolerance)
"""

import argparse
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import List, Optional


REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SOLVER = os.path.join(REPO, "build", "sharpSAT")
TEMP_CNF = os.path.normpath(os.path.join(REPO, "..", "temp_cnf"))


@dataclass
class CalibrationCase:
    name: str
    cnf_filename: str             # relative to TEMP_CNF
    flags: str
    expected_count: str           # exact decimal integer as string; "?" = unset
    expected_wall_s: float        # seconds; 0.0 = unset
    timeout_s: float              # subprocess timeout (give 2x expected_wall)


# ---------------------------------------------------------------------
# Manifest. Counts come from benchmark_log.md / portfolio_insights.md or
# from a --bootstrap run on a known-good binary. Update via --bootstrap.
#
# Mix is tuned for ~30s total wall: a few fast instances exercising
# different mechanisms, plus one density-1 structured case (t1_041) at
# the cheap config to verify reactive METIS aggressive still rescues.
# ---------------------------------------------------------------------
CASES: List[CalibrationCase] = [
    CalibrationCase(
        name="t1_065_plain",
        cnf_filename="mc2025_track1_065.cnf",
        flags="-rec -sep 5 -cb 3 -sepMode metis",
        expected_count="37778931862957161709568",
        expected_wall_s=0.02,
        timeout_s=10.0,
    ),
    CalibrationCase(
        name="t1_071_plain",
        cnf_filename="mc2025_track1_071.cnf",
        flags="-rec -sep 5 -cb 3 -sepMode metis",
        expected_count="456295684783698132731653351484293780287639045166077370506304563500761788632102076272640",
        expected_wall_s=0.39,
        timeout_s=10.0,
    ),
    CalibrationCase(
        name="t1_011_plain",
        cnf_filename="mc2025_track1_011.cnf",
        flags="-rec -sep 5 -cb 3 -sepMode metis",
        expected_count="536870912306",
        expected_wall_s=13.66,
        timeout_s=60.0,
    ),
    CalibrationCase(
        name="t1_021_k10_s1_clausesFirst",
        cnf_filename="mc2025_track1_021_k10_s1.cnf",
        flags="-rec -sep 5 -cb 3 -sepMode metis -sepClausesFirst",
        expected_count="430052389882336036",
        expected_wall_s=4.35,
        timeout_s=20.0,
    ),
    CalibrationCase(
        name="t1_041_react_agg_no_anchor",
        cnf_filename="mc2025_track1_041.cnf",
        flags=("-rec -sep 5 -cb 3 -sepMode metis -wlIter 2 "
               "-reactiveMetis -reactiveMetisMin 10 -reactiveMetisSkip 4"),
        expected_count="55167673345665723920748518509559191427126157713600668747551155984464814565852429214532890871597581092760820761433899035366724021426276206778784476956728545794415588517340665239809770460152011863941472087189967805273376751508436564089844981127155486259410453216757980597598419691809480935938892302729473961724143274899786192544271057393556788255628500088140128945500769427718657635074960261647187237142803335120990857666915230084057082925443259892051805732864",
        expected_wall_s=10.48,
        timeout_s=60.0,
    ),
]


# Solver output parsers.
# `# solutions\n<COUNT>\n# END` — count is on the line after "# solutions".
_COUNT_BLOCK = re.compile(r"^# solutions\s*\n(\d+)\s*\n# END\s*$",
                          re.MULTILINE)
_TIME = re.compile(r"^time:\s*([\d.]+)\s*s", re.MULTILINE)
_TIMEOUT = re.compile(r"TIMEOUT\s*!")


@dataclass
class RunResult:
    case_name: str
    count: Optional[str]
    wall_s: Optional[float]
    finished: bool
    raw_stderr_tail: str


def run_case(case: CalibrationCase, solver: str) -> RunResult:
    cnf_path = os.path.join(TEMP_CNF, case.cnf_filename)
    if not os.path.exists(cnf_path):
        return RunResult(case_name=case.name, count=None, wall_s=None,
                         finished=False,
                         raw_stderr_tail=f"CNF not found: {cnf_path}")
    cmd = [solver] + case.flags.split() + [cnf_path]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=case.timeout_s + 5.0)
    except subprocess.TimeoutExpired:
        return RunResult(case_name=case.name, count=None, wall_s=None,
                         finished=False,
                         raw_stderr_tail="subprocess.TimeoutExpired")
    out = (proc.stdout or "") + "\n" + (proc.stderr or "")
    finished = (_TIMEOUT.search(out) is None)
    m = _COUNT_BLOCK.search(out)
    count = m.group(1) if m else None
    m = _TIME.search(out)
    wall = float(m.group(1)) if m else None
    err_tail = (proc.stderr or "").splitlines()[-3:]
    return RunResult(case_name=case.name, count=count, wall_s=wall,
                     finished=finished, raw_stderr_tail=" | ".join(err_tail))


def wall_within_tolerance(actual: float, expected: float) -> bool:
    """Tolerance: max(1 sec, 20% of expected)."""
    tolerance = max(1.0, 0.20 * expected)
    return abs(actual - expected) <= tolerance


def check_case(case: CalibrationCase, result: RunResult) -> tuple[bool, str]:
    """Return (passed, status_message)."""
    if result.count is None:
        return False, f"FAIL: no count in output ({result.raw_stderr_tail})"
    if not result.finished:
        return False, f"FAIL: TIMEOUT marker present ({result.raw_stderr_tail})"

    issues = []
    if case.expected_count != "?" and result.count != case.expected_count:
        # Show first/last 20 chars to keep message short for huge counts.
        def short(s: str) -> str:
            if len(s) <= 50: return s
            return f"{s[:20]}...{s[-20:]} (len={len(s)})"
        issues.append(f"COUNT MISMATCH: expected {short(case.expected_count)}, "
                      f"got {short(result.count)}")

    if case.expected_wall_s > 0 and result.wall_s is not None:
        if not wall_within_tolerance(result.wall_s, case.expected_wall_s):
            tol = max(1.0, 0.20 * case.expected_wall_s)
            sign = "slower" if result.wall_s > case.expected_wall_s else "faster"
            issues.append(f"WALL REGRESSION: expected {case.expected_wall_s:.2f}s "
                          f"(±{tol:.2f}s), got {result.wall_s:.2f}s "
                          f"({sign})")

    if issues:
        return False, "FAIL: " + "; ".join(issues)
    return True, f"PASS ({result.wall_s:.2f}s)"


def print_bootstrap(results: List[RunResult]) -> None:
    """Emit suggested manifest values for paste-back into this file."""
    print("\n=== BOOTSTRAP VALUES (paste into CASES manifest above) ===")
    for r in results:
        if r.count is None:
            print(f"  {r.case_name}: no count parsed")
            continue
        wall = r.wall_s if r.wall_s is not None else 0.0
        # Round wall up to next 0.1s for stable expected.
        print(f"  {r.case_name}:")
        print(f"    expected_count=\"{r.count}\",")
        print(f"    expected_wall_s={wall:.2f},")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--solver", default=DEFAULT_SOLVER,
                    help=f"sharpSAT binary (default: {DEFAULT_SOLVER})")
    ap.add_argument("--bootstrap", action="store_true",
                    help="Record actuals; print as suggested manifest values")
    args = ap.parse_args()

    if not os.path.exists(args.solver):
        sys.exit(f"solver not found: {args.solver}")

    print(f"calibration_run: solver={args.solver}")
    print(f"                 cases={len(CASES)}")
    print(f"                 mode={'BOOTSTRAP' if args.bootstrap else 'VERIFY'}")
    print()

    results: List[RunResult] = []
    pass_count = 0
    fail_count = 0
    t0 = time.time()

    for i, case in enumerate(CASES, 1):
        print(f"[{i}/{len(CASES)}] {case.name} ... ", end="", flush=True)
        result = run_case(case, args.solver)
        results.append(result)
        if args.bootstrap:
            wall = result.wall_s if result.wall_s is not None else -1
            count_short = (result.count[:20] + "..." + result.count[-20:]
                           if result.count and len(result.count) > 50
                           else (result.count or "?"))
            print(f"BOOTSTRAP wall={wall:.2f}s count={count_short}")
        else:
            ok, msg = check_case(case, result)
            print(msg)
            if ok: pass_count += 1
            else:  fail_count += 1

    total_wall = time.time() - t0
    print(f"\nTotal wall: {total_wall:.1f}s")

    if args.bootstrap:
        print_bootstrap(results)
        return 0

    print(f"\nResult: {pass_count}/{len(CASES)} passed, {fail_count} failed")
    return 0 if fail_count == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
