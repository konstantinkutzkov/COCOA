#!/usr/bin/env python3
"""Step 1 of the selection pipeline: choose the SOLVER (structure-based).

Two clear-cut rules where there is no need to run both solvers:

  1. METIS finds a SMALL ENOUGH separator            -> COCOA (sharpsat-separator)
  2. else, Arjun SUBSTANTIALLY REDUCES the formula   -> Ganak (ganak-canonical)
  3. else (neither)                                  -> UNDECIDED
       (the hard case: deferred to step 2 — a progress-raced portfolio of
        config archetypes; this router just flags it and logs the probes.)

Probes run LAZILY: the (more expensive, time-budgeted) Arjun probe only runs
when METIS did not already route to COCOA. Step 1 returns SOLVER + a default
profile; step 2 refines the profile (hashing etc.). Every decision + its probe
signals are logged for the flywheel.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from preprocess import metis_wrapper, arjun_wrapper  # noqa: E402
from classifier.thresholds import SEP_RATIO_MAX, BALANCE_MIN  # noqa: E402

_REPO_ROOT = Path(__file__).resolve().parent.parent

# --- knobs (tunable; PROVISIONAL defaults — not yet calibrated, do not commit blind) ---
METIS_TIMEOUT_S = 30.0
INDEP_FRACTION_MAX = 0.50  # "substantial": independent support |I| <= 50% of vars
ARJUN_STALL_S = 5.0        # W: bail if |I| hasn't dropped in this long
ARJUN_CAP_S = 60.0         # T: hard wall backstop on the probe

COCOA = "sharpsat-separator"
GANAK = "ganak-canonical"


def small_separator(metis: dict) -> tuple[bool, str]:
    """COCOA's own Phase-2 acceptance gate: sep_ratio small AND reasonably balanced."""
    if metis.get("metis_status") != "ok":
        return False, f"metis_status={metis.get('metis_status')}"
    sr = metis.get("metis_sep_ratio")
    bal = metis.get("metis_balance")
    if not isinstance(sr, (int, float)) or not isinstance(bal, (int, float)):
        return False, "metis_sep_ratio/balance missing"
    ok = (sr <= SEP_RATIO_MAX) and (bal >= BALANCE_MIN)
    return ok, f"sep_ratio={sr:.3f}(<= {SEP_RATIO_MAX}), balance={bal:.3f}(>= {BALANCE_MIN})"


def substantial_reduction(arj: dict) -> tuple[bool, str]:
    """Substantial iff Arjun's independent-support fraction |I|/n is small enough.

    |I| (Arjun's `new size`/`sampl` token) is reported live throughout the run, so
    we have a valid estimate no matter WHY the probe stopped (success/stall/cap/finished).
    |I| is monotone non-increasing, so the min seen is a sound upper bound on what
    Arjun reached — if it is already small enough, letting Arjun run longer would
    only shrink it further, so the verdict can't flip from substantial to not.
    """
    ov = arj.get("orig_vars") or 0
    indep = arj.get("indep_size")
    frac = arj.get("indep_frac")
    reason = arj.get("stop_reason")
    if frac is None or indep is None or ov <= 0:
        return False, f"no |I| signal (stop_reason={reason})"
    ok = frac <= INDEP_FRACTION_MAX
    return ok, (f"indep_fraction={frac:.3f} (|I|={indep}/{ov}, <= {INDEP_FRACTION_MAX}); "
                f"stop_reason={reason}")


def _decision(solver, reason, stage, metis=None, arjun=None) -> dict:
    return {
        "ts": _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "solver": solver,            # None == UNDECIDED
        "profile": "default" if solver else None,   # step 2 refines (hash etc.)
        "stage": stage,
        "reason": reason,
        "metis": metis,
        "arjun": _arjun_summary(arjun),
    }


def _arjun_summary(arj: dict | None) -> dict | None:
    if arj is None:
        return None
    # Trim the trajectory in the log to first+last few points (keep it small).
    traj = arj.get("trajectory") or []
    return {k: arj[k] for k in ("orig_vars", "indep_size", "indep_frac",
                                "stop_reason", "wall_s") if k in arj} \
        | {"traj_points": len(traj),
           "traj_head": traj[:3], "traj_tail": traj[-3:]}


def select_solver(cnf: str, arjun_stall_s: float = ARJUN_STALL_S,
                  arjun_cap_s: float = ARJUN_CAP_S,
                  metis_timeout_s: float = METIS_TIMEOUT_S) -> dict:
    """Run the two structure-based rules lazily and return a decision dict."""
    # --- rule 1: small METIS separator -> COCOA ---
    metis = None
    try:
        metis = metis_wrapper.run(cnf, timeout_s=metis_timeout_s)
    except metis_wrapper.MetisHelperMissing as e:
        metis = {"metis_status": "helper_missing", "error": str(e)}
    except metis_wrapper.MetisTimeout:
        metis = {"metis_status": "timeout"}
    except metis_wrapper.MetisError as e:
        metis = {"metis_status": "error", "error": str(e), **(e.partial or {})}
    except Exception as e:  # noqa: BLE001 — any unexpected probe failure degrades
        metis = {"metis_status": "error", "error": str(e)}

    if metis.get("metis_status") == "too_small":
        return _decision(COCOA, "metis_too_small (trivial); COCOA default", "metis", metis)

    small, det = small_separator(metis)
    if small:
        return _decision(COCOA, f"small separator: {det}", "metis", metis)

    # --- rule 2: Arjun substantially reduces -> Ganak (lazy: only reached here) ---
    # Probe is TRIAGE only: Ganak re-runs its own Arjun if routed here (no reuse).
    try:
        arj = arjun_wrapper.run(cnf, stall_s=arjun_stall_s, cap_s=arjun_cap_s,
                                success_indep_frac=INDEP_FRACTION_MAX)
    except arjun_wrapper.ArjunHelperMissing as e:
        # Mirror the METIS degrade: no probe -> can't claim reduction -> UNDECIDED.
        return _decision(None, f"neither: sep({det}); arjun helper missing: {e} -> UNDECIDED",
                         "undecided", metis, {"stop_reason": "helper_missing"})
    subst, det2 = substantial_reduction(arj)
    if subst:
        return _decision(GANAK, f"not-small-sep ({det}); substantial Arjun reduction: {det2}",
                         "arjun", metis, arj)

    # --- rule 3: neither -> UNDECIDED (hand to step 2) ---
    return _decision(None, f"neither: sep({det}); arjun({det2}) -> UNDECIDED",
                     "undecided", metis, arj)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Step-1 solver selection (METIS sep / Arjun reduction).")
    ap.add_argument("cnf")
    ap.add_argument("--arjun-stall", type=float, default=ARJUN_STALL_S,
                    help="W: bail if |I| hasn't dropped in this many seconds")
    ap.add_argument("--arjun-cap", type=float, default=ARJUN_CAP_S,
                    help="T: hard wall backstop on the Arjun probe")
    ap.add_argument("--log", default=str(_REPO_ROOT / "portfolio" / "selection_log.jsonl"))
    args = ap.parse_args(argv)

    if not os.environ.get("PORTFOLIO_METIS_FEATURES_BIN"):
        os.environ["PORTFOLIO_METIS_FEATURES_BIN"] = str(
            _REPO_ROOT / "cocoa" / "build" / "metis_features")

    d = select_solver(args.cnf, arjun_stall_s=args.arjun_stall, arjun_cap_s=args.arjun_cap)
    d["cnf"] = args.cnf
    try:
        with open(args.log, "a") as fh:
            fh.write(json.dumps(d, default=str) + "\n")
    except OSError as e:
        print(f"WARNING: could not append selection log: {e}", file=sys.stderr)

    sel = d["solver"] or "UNDECIDED"
    print(f"=== step-1 selection: {os.path.basename(args.cnf)} ===")
    if d["metis"]:
        m = d["metis"]
        print(f"  metis : status={m.get('metis_status')} "
              f"sep_ratio={m.get('metis_sep_ratio')} balance={m.get('metis_balance')}")
    if d["arjun"]:
        a = d["arjun"]
        frac = a.get("indep_frac")
        frac_s = f"{frac:.3f}" if isinstance(frac, (int, float)) else "n/a"
        print(f"  arjun : orig_vars={a.get('orig_vars')} |I|={a.get('indep_size')} "
              f"frac={frac_s} stop={a.get('stop_reason')} "
              f"wall={a.get('wall_s')}s traj_pts={a.get('traj_points')}")
    print(f"  --> {sel}   ({d['reason']})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
