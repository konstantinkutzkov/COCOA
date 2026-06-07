#!/usr/bin/env python3
"""Step 1 of the selection pipeline: FEATURE EXTRACTION (no hard verdict).

The engine choice (COCOA vs Ganak) must NOT be a binary decision off a single
structural number — neither the METIS separator nor the Arjun reduction. Both
are SIGNALS. Step 1 computes them; step 2 (the progress-raced portfolio) uses
them to pick which/how many COCOA archetypes to race and how eagerly to hand off
to Ganak. COCOA-vs-Ganak falls out of the race + fallback, not a gate here.

  - nd_cost band (whole ND tree)   -> WHICH COCOA archetypes + Ganak-handoff budget
      low  : precomputed sep validated 7/7 -> sep configs, patient
      mid  : sep/reactive/nosep all occur  -> diverse trio
      high : mostly hopeless, but a short sep probe can win (t1_011) -> bail fast
  - Arjun reduction signal         -> strong reduction = Ganak-favorable, hand off sooner

  (See race/archetypes.py::race_plan for the band+signal -> race mapping, and
  project_nd_cost_signal for why a caching-blind bits-gate mis-routes t1_011.)

Arjun is skipped on low-band instances (COCOA-sep finishes before handoff matters).
select_solver always returns solver=None; the decision record carries the features.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from preprocess import metis_wrapper, arjun_wrapper, nd_cost_wrapper  # noqa: E402
from classifier.thresholds import (  # noqa: E402
    SEP_RATIO_MAX, BALANCE_MIN, ND_BAND_LOW_MAX, ND_BAND_HIGH_MIN)

_REPO_ROOT = Path(__file__).resolve().parent.parent

# --- knobs (tunable; PROVISIONAL defaults — not yet calibrated, do not commit blind) ---
METIS_TIMEOUT_S = 30.0
INDEP_FRACTION_MAX = 0.50  # "substantial": independent support |I| <= 50% of vars
ARJUN_STALL_S = 5.0        # W: bail if |I| hasn't dropped in this long
ARJUN_CAP_S = 60.0         # T: hard wall backstop on the probe

COCOA = "sharpsat-separator"
GANAK = "ganak-canonical"


def nd_band(nd: dict) -> tuple[str, str]:
    """Classify by whole-ND-tree branching cost into a band that informs STEP 2
    (which COCOA archetypes to race, how many, how long before handing to Ganak).

    This is deliberately NOT a binary COCOA/Ganak gate. Validation (2026-06-04,
    portfolio/nd_crosstab.py) showed a caching-blind bits-threshold mis-routes:
    t1_011 scores 208 bits yet is an 8.8s precomputed-sep win. The band -> strategy
    rule held only in the LOW band (precomputed sep, 7/7); above it caching (which
    nd_cost can't see) dominates. So the cost is a PRIOR for step 2, not a verdict.

    Returns (band, detail). band in {low, mid, high, unknown}.
    """
    st = nd.get("nd_status")
    if st == "too_small":
        return "low", "nd_too_small (trivial)"
    if st != "ok":
        return "unknown", f"nd_status={st}"
    cost = nd.get("nd_log2_cost")
    if not isinstance(cost, (int, float)):
        return "unknown", "nd_log2_cost missing"
    if cost <= ND_BAND_LOW_MAX:
        b = "low"
    elif cost >= ND_BAND_HIGH_MIN:
        b = "high"
    else:
        b = "mid"
    return b, f"nd_log2_cost={cost:.1f} -> band={b}"


def small_separator(metis: dict) -> tuple[bool, str]:
    """DEPRECATED single-root-cut gate (kept for reference/logging only; the
    router now decides with cocoa_feasible()). sep_ratio small AND balanced."""
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


def _decision(solver, reason, stage, metis=None, arjun=None, nd=None, band=None,
              arjun_substantial=False) -> dict:
    return {
        "ts": _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "solver": solver,            # None == no step-1 verdict (step 2 decides)
        "profile": "default" if solver else None,   # step 2 refines (hash etc.)
        "stage": stage,
        "reason": reason,
        "nd": nd,                    # whole-ND-tree branching cost (feature, not a verdict)
        "nd_band": band,             # low/mid/high -> step-2 archetype selection + handoff
        "arjun_substantial": arjun_substantial,   # signal: Ganak-favorable -> hand off sooner
        "metis": metis,              # single-root cut, kept for reference/logging
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
    """Step 1 = FEATURE EXTRACTION (no hard COCOA/Ganak verdict).

    Computes the nd_cost band and the Arjun reduction signal. Both are SIGNALS for
    step 2 (the race), which uses them to pick which/how many COCOA archetypes to
    run and how eagerly to hand off to Ganak. select_solver always returns
    solver=None now — the engine choice falls out of the race + fallback, never a
    single structural number (a small separator OR a strong Arjun reduction).
    """
    # METIS single-root cut: kept for the log record (sep_ratio/balance) only.
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

    # nd_cost (whole ND tree) -> band: the step-2 archetype-selection signal.
    nd = None
    try:
        nd = nd_cost_wrapper.run(cnf)
    except nd_cost_wrapper.NdCostHelperMissing as e:
        nd = {"nd_status": "helper_missing", "error": str(e)}
    except nd_cost_wrapper.NdCostTimeout:
        nd = {"nd_status": "timeout"}
    except nd_cost_wrapper.NdCostError as e:
        nd = {"nd_status": "error", "error": str(e), **(e.partial or {})}
    except Exception as e:  # noqa: BLE001 — any unexpected probe failure degrades
        nd = {"nd_status": "error", "error": str(e)}
    band, det = nd_band(nd)

    # Arjun reduction is a SIGNAL for step-2 handoff timing (strong reduction ->
    # Ganak-favorable -> hand off sooner), NOT a route. Skip it on low-band
    # instances: COCOA-sep is validated there and they finish before handoff
    # matters, so a bounded-but-real Arjun probe would be wasted work.
    arj = None
    subst = False
    det2 = "skipped (low band)"
    if band != "low":
        try:
            arj = arjun_wrapper.run(cnf, stall_s=arjun_stall_s, cap_s=arjun_cap_s,
                                    success_indep_frac=INDEP_FRACTION_MAX)
        except arjun_wrapper.ArjunHelperMissing:
            arj = {"stop_reason": "helper_missing"}
        subst, det2 = substantial_reduction(arj)

    reason = f"features: nd_band={band} ({det}); arjun_substantial={subst} ({det2})"
    return _decision(None, reason, "features", metis, arj, nd=nd, band=band,
                     arjun_substantial=subst)


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
    if not os.environ.get("PORTFOLIO_NDCOST_BIN"):
        os.environ["PORTFOLIO_NDCOST_BIN"] = str(
            _REPO_ROOT / "cocoa" / "build" / "nd_cost")

    d = select_solver(args.cnf, arjun_stall_s=args.arjun_stall, arjun_cap_s=args.arjun_cap)
    d["cnf"] = args.cnf
    try:
        with open(args.log, "a") as fh:
            fh.write(json.dumps(d, default=str) + "\n")
    except OSError as e:
        print(f"WARNING: could not append selection log: {e}", file=sys.stderr)

    sel = d["solver"] or "UNDECIDED"
    print(f"=== step-1 selection: {os.path.basename(args.cnf)} ===")
    if d.get("nd"):
        n = d["nd"]
        print(f"  nd    : status={n.get('nd_status')} "
              f"log2_cost={n.get('nd_log2_cost')} max_path={n.get('nd_max_path_sep')} "
              f"root_sep={n.get('nd_root_sep_vars')} worst_leaf={n.get('nd_worst_leaf_vars')}")
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
