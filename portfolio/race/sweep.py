"""Differential validation sweep for the end-to-end pipeline — STREAMING.

For each instance: run the pipeline (step1 -> race -> count) under a budget, then
VERIFY the count against an independent reference — a documented ground-truth if
known, else `ganak --prob 0` (deterministic exact). Any MISMATCH is a critical
soundness bug; TIMEOUT just means the instance needs more budget.

Live visibility (for multi-day runs): every solver-option window result and every
instance verdict streams to stdout AS IT HAPPENS (flushed), and a durable
per-instance record is appended+flushed to a JSONL so problems are caught early.
Run with `python -u` (or PYTHONUNBUFFERED=1) when redirecting to a file.

Usage: python -u race/sweep.py [--budget S] [--jsonl path] <inst...>
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from pipeline import run_pipeline                         # noqa: E402
from race.archetypes import ganak_bin                     # noqa: E402

CNF_DIR = os.environ.get(
    "PORTFOLIO_CNF_DIR",
    "/Users/konstantin.kutzkov/Desktop/Code/SharpSAT/temp_cnf")
_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Documented, cross-confirmed ground truth (benchmark_log / prior verification).
KNOWN = {
    "065": "37778931862957161709568",
    "071": "456295684783698132731653351484293780287639045166077370506304563500761788632102076272640",
    "011": "536870912306",
    "159": "2779865831606277536",
    "049": "8695763196077742",
    "045": "132951278067432",
    "041": ("55167673345665723920748518509559191427126157713600668747551155984464814565852429214532890871597"
            "581092760820761433899035366724021426276206778784476956728545794415588517340665239809770460152011"
            "863941472087189967805273376751508436564089844981127155486259410453216757980597598419691809480935"
            "938892302729473961724143274899786192544271057393556788255628500088140128945500769427718657635074"
            "960261647187237142803335120990857666915230084057082925443259892051805732864"),
}

_GREF = re.compile(r"c s exact arb int\s+(\d+)")


def _now():
    return _dt.datetime.now().replace(microsecond=0).isoformat()


def _say(msg):
    print(msg, flush=True)


def ganak_ref(cnf: str, timeout_s: float) -> str | None:
    """Independent exact reference; None if it didn't finish / errored."""
    try:
        p = subprocess.run([ganak_bin(), "--prob", "0", cnf],
                           capture_output=True, text=True, timeout=timeout_s)
    except (subprocess.TimeoutExpired, OSError):
        return None
    m = _GREF.search(p.stdout)
    return m.group(1) if m else None


def sweep(insts, budget, round1, round2, ref_timeout, jsonl_path):
    rows = []
    jf = open(jsonl_path, "a")
    _say(f"# sweep start {_now()} | budget={budget}s round1={round1} round2={round2} "
         f"| jsonl={jsonl_path}")
    for n in insts:
        cnf = os.path.join(CNF_DIR, f"mc2025_track1_{n}.cnf")
        if not os.path.exists(cnf):
            _say(f"t1_{n}: MISSING {cnf}")
            continue
        _say(f"\n########## t1_{n}  [{_now()}] ##########")
        t0 = time.monotonic()
        try:
            r = run_pipeline(cnf, budget_s=budget, round1_s=round1, round2_s=round2,
                             progress_interval=5.0, log=_say)
        except Exception as e:  # noqa: BLE001 — one bad instance shouldn't kill the sweep
            _say(f"  PIPELINE ERROR: {type(e).__name__}: {e}")
            rec = {"ts": _now(), "inst": f"t1_{n}", "status": "error", "error": str(e)[:200]}
            jf.write(json.dumps(rec) + "\n"); jf.flush()
            rows.append((n, "ERROR", "-", "-", round(time.monotonic() - t0), str(e)[:40]))
            continue
        wall = round(time.monotonic() - t0)
        got = str(r.get("count")) if r.get("status") == "solved" else None

        if got is None:
            verdict = f"no-count ({r.get('status')})"
        elif n in KNOWN:
            verdict = "MATCH(known) ✓" if got == KNOWN[n] else "MISMATCH(known) ✗✗✗"
        else:
            ref = ganak_ref(cnf, ref_timeout)
            if ref is None:
                verdict = "unverified (ref timeout)"
            elif ref == got:
                verdict = "MATCH(ganak-ref) ✓"
            else:
                verdict = f"MISMATCH(ganak-ref) ✗✗✗ ref={ref}"

        rec = {"ts": _now(), "inst": f"t1_{n}", "route": r.get("step1_solver"),
               "winner": r.get("winner"), "status": r.get("status"),
               "round": r.get("round"), "wall_s": wall, "count": got,
               "verdict": verdict}
        jf.write(json.dumps(rec, default=str) + "\n"); jf.flush()
        _say(f"  ==> t1_{n}: route={r.get('step1_solver')} winner={r.get('winner')} "
             f"status={r.get('status')} wall={wall}s  VERDICT: {verdict}")
        rows.append((n, r.get("step1_solver"), r.get("winner"),
                     r.get("status"), wall, verdict))
    jf.close()
    return rows


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("insts", nargs="*", default=["065", "071", "011", "041", "159"])
    ap.add_argument("--budget", type=float, default=120.0)
    ap.add_argument("--round1", type=float, default=20.0)
    ap.add_argument("--round2", type=float, default=15.0)
    ap.add_argument("--ref-timeout", type=float, default=90.0)
    ap.add_argument("--jsonl", default=os.path.join(_REPO, "sweep_log.jsonl"))
    a = ap.parse_args(argv)
    rows = sweep(a.insts, a.budget, a.round1, a.round2, a.ref_timeout, a.jsonl)

    _say("\n" + "=" * 78)
    _say(f"{'inst':<6}{'route':<22}{'winner':<22}{'status':<9}{'wall':<6}verdict")
    _say("-" * 78)
    mism = 0
    for n, route, winner, status, wall, verdict in rows:
        rr = (route or "-").replace("sharpsat-separator", "COCOA").replace("ganak-canonical", "Ganak") if route else "-"
        _say(f"t1_{n:<3}{str(rr):<22}{str(winner):<22}{str(status):<9}{str(wall):<6}{verdict}")
        if "MISMATCH" in verdict:
            mism += 1
    _say("=" * 78)
    _say(f"{len(rows)} instances; {mism} MISMATCH" + (" !!!" if mism else " (no soundness issues)"))
    return 1 if mism else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
