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

# --- knobs (tunable; agreed defaults) ---
ARJUN_BUDGET_S = 30.0      # wall budget for the Arjun reduction probe
METIS_TIMEOUT_S = 30.0
INDEP_FRACTION_MAX = 0.50  # "substantial": independent support |I| <= 50% of vars
VAR_REDUCTION_MIN = 0.50   # fallback (e.g. on Arjun timeout): vars halved within budget

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
    """Arjun reduction is substantial if the counting problem at least halves.

    Primary signal (Arjun completed): independent-support fraction |I|/nvars.
    Fallback (Arjun timed out): variable reduction achieved within the budget.
    """
    ov = arj.get("orig_vars") or 0
    indep = arj.get("indep_size")
    best_vars = arj.get("best_vars") or ov
    var_red = (1.0 - best_vars / ov) if ov > 0 else 0.0

    if arj.get("completed") and indep is not None and ov > 0:
        frac = indep / ov
        ok = frac <= INDEP_FRACTION_MAX
        return ok, f"indep_fraction={frac:.3f} (|I|={indep}/{ov}, <= {INDEP_FRACTION_MAX}); var_red={var_red:.2f}"
    # timed out (or no |I|): lean on the achieved variable reduction
    ok = var_red >= VAR_REDUCTION_MIN
    tag = "timed_out" if arj.get("timed_out") else "no_indep"
    return ok, f"{tag}: var_reduction={var_red:.3f} (>= {VAR_REDUCTION_MIN}); best_vars={best_vars}/{ov}"


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
    return {k: arj[k] for k in ("orig_vars", "orig_cls", "best_vars", "best_cls",
                                "simp_vars", "simp_cls", "indep_size", "multiplier",
                                "wall_s", "completed", "timed_out") if k in arj} \
        | {"traj_points": len(traj),
           "traj_head": traj[:3], "traj_tail": traj[-3:]}


def select_solver(cnf: str, arjun_budget_s: float = ARJUN_BUDGET_S,
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

    if metis.get("metis_status") == "too_small":
        return _decision(COCOA, "metis_too_small (trivial); COCOA default", "metis", metis)

    small, det = small_separator(metis)
    if small:
        return _decision(COCOA, f"small separator: {det}", "metis", metis)

    # --- rule 2: Arjun substantially reduces -> Ganak (lazy: only reached here) ---
    arj = arjun_wrapper.run(cnf, time_budget_s=arjun_budget_s)
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
    ap.add_argument("--arjun-budget", type=float, default=ARJUN_BUDGET_S)
    ap.add_argument("--log", default=str(_REPO_ROOT / "portfolio" / "selection_log.jsonl"))
    args = ap.parse_args(argv)

    if not os.environ.get("PORTFOLIO_METIS_FEATURES_BIN"):
        os.environ["PORTFOLIO_METIS_FEATURES_BIN"] = str(
            _REPO_ROOT / "cocoa" / "build" / "metis_features")

    d = select_solver(args.cnf, arjun_budget_s=args.arjun_budget)
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
        print(f"  arjun : orig_vars={a.get('orig_vars')} best_vars={a.get('best_vars')} "
              f"|I|={a.get('indep_size')} completed={a.get('completed')} "
              f"timed_out={a.get('timed_out')} wall={a.get('wall_s')}s "
              f"traj_pts={a.get('traj_points')}")
    print(f"  --> {sel}   ({d['reason']})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
