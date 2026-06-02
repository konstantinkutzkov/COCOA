#!/usr/bin/env python3
"""Two-solver portfolio main driver.

Flow:
  1. Parse CLI args (input.cnf + optional extra-flags for chosen solver).
  2. Extract cheap structural features (features.py).
  3. Decide solver + profile (classifier/thresholds.py).
  4. Exec the chosen solver via solvers/<chosen>.py.
  5. Append a decision record to decisions.jsonl (always — even on failure).
  6. Print the count to stdout; exit 0 on success, 1 on failure.

Logged decision record (JSON-per-line) drives the data flywheel that will
replace thresholds with a learned classifier once enough labels accrue.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import sys
from pathlib import Path

import features
import solvers
from classifier import decide

_DECISIONS_LOG = Path(__file__).resolve().parent / "decisions.jsonl"


def _utc_now_iso() -> str:
    # Inline ISO-Z formatting; avoids depending on tz config.
    return (_dt.datetime.now(_dt.timezone.utc)
            .replace(microsecond=0)
            .isoformat()
            .replace("+00:00", "Z"))


def _append_decision(record: dict) -> None:
    """Best-effort append to the telemetry log.

    Failures to write the log must not affect the user-visible exit
    behavior of the driver; we print a warning and continue.
    """
    try:
        with open(_DECISIONS_LOG, "a") as fh:
            fh.write(json.dumps(record, default=str) + "\n")
    except OSError as e:
        print(f"WARNING: failed to append to {_DECISIONS_LOG}: {e}",
              file=sys.stderr)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Two-solver portfolio. Routes a CNF to the better-fit "
                    "solver and propagates the count.",
        usage="%(prog)s [-t SECONDS] <input.cnf> [-- extra-flags...]",
    )
    ap.add_argument("input_cnf", help="Path to the input CNF file.")
    ap.add_argument("-t", "--time-budget", type=int, default=None,
                    help="Wall-clock budget in seconds for the chosen solver.")
    ap.add_argument("extra_flags", nargs="*",
                    help="Extra flags passed to the chosen solver verbatim.")
    args = ap.parse_args(argv)

    input_path = os.path.abspath(args.input_cnf)
    if not os.path.exists(input_path):
        print(f"ERROR: input file does not exist: {input_path}", file=sys.stderr)
        return 1

    feats = features.extract(input_path)
    decision = decide(feats)

    record: dict = {
        "ts": _utc_now_iso(),
        "input": input_path,
        "features": feats.to_dict(),
        "decision": decision.to_dict(),
    }

    result = None
    try:
        result = solvers.run(
            solver_name=decision.solver,
            profile=decision.profile,
            input_cnf=input_path,
            extra_flags=args.extra_flags,
            time_budget_s=args.time_budget,
        )
        record["outcome"] = {
            "exit": result.exit_code,
            "wall_time_s": round(result.wall_time_s, 3),
            "count": result.count,
        }
    except solvers.SolverError as e:
        record["outcome"] = {"exit": -1, "error": str(e)}
    except Exception as e:
        # Re-raise after recording — internal bugs should still propagate
        # for visibility, but the telemetry record is captured first.
        record["outcome"] = {"exit": -2, "error": f"{type(e).__name__}: {e}"}
        _append_decision(record)
        raise

    _append_decision(record)

    if result is None or not result.ok:
        # On failure, dump the chosen solver's stderr to ours so the user
        # sees what went wrong.
        if result is not None:
            sys.stderr.write(result.stderr)
        return 1

    # Success: count to stdout, exit 0.
    print(result.count)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
