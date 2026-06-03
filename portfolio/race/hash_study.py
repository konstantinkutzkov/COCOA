"""Validate the dive's Ganak-hash recommendation against ACTUAL Ganak performance.

For each instance: run Ganak NATIVE (--prob 0) and Ganak CANONICAL (--cachehash
canonical), time both (--maxcache 26000), get the dive recommendation, and check
whether the dive picked the faster hash. Ganak is memory-hungry (26 GB) so runs
are SEQUENTIAL. Streams live (run with `python -u`) + appends a JSONL.

This answers two questions before we wire the dive into the pipeline:
  1. Does the hash choice actually matter (native vs canonical spread)?
  2. Does the dive predict the faster hash?

Usage: python -u race/hash_study.py [--budget T] [--warm S] <full_cnf_paths...>
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
from race.archetypes import ganak_bin, cocoa_bin          # noqa: E402
from calibrate import calibrate_hash                       # noqa: E402

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # portfolio/ (for logs)
_GREF = re.compile(r"c s exact arb int\s+(\d+)")


def _say(m):
    print(m, flush=True)


def _now():
    return _dt.datetime.now().replace(microsecond=0).isoformat()


def run_ganak(cnf, flags, timeout_s):
    t0 = time.monotonic()
    try:
        p = subprocess.run([ganak_bin(), *flags, cnf],
                           capture_output=True, text=True, timeout=timeout_s)
    except (subprocess.TimeoutExpired, OSError):
        return {"status": "timeout", "wall_s": round(time.monotonic() - t0, 1), "count": None}
    m = _GREF.search(p.stdout)
    return {"status": "solved" if m else "no-count",
            "wall_s": round(time.monotonic() - t0, 1),
            "count": m.group(1) if m else None}


def _study_one(cnf, name, a, jf, rows):
    native = run_ganak(cnf, ["--prob", "0", "--maxcache", a.maxcache], a.budget)
    _say(f"  native    {native}")
    canon = run_ganak(cnf, ["--cachehash", "canonical", "--wliter", "2",
                            "--maxcache", a.maxcache], a.budget)
    _say(f"  canonical {canon}")
    dive = calibrate_hash(cnf, n_dives=2000, warm_s=a.warm, seed=12345)
    rec = dive["recommended_hash"]                 # identity | canonical

    # actual winner (faster of the two that solved); soundness cross-check
    ns, cs = native["status"] == "solved", canon["status"] == "solved"
    agree = (ns and cs and native["count"] == canon["count"])
    if ns and cs:
        winner = "native" if native["wall_s"] <= canon["wall_s"] else "canonical"
        spread = round(abs(native["wall_s"] - canon["wall_s"]), 1)
    elif ns:
        winner, spread = "native", None
    elif cs:
        winner, spread = "canonical", None
    else:
        winner, spread = None, None
    rec_engine = "native" if rec == "identity" else "canonical"  # identity<->native
    dive_correct = (None if winner is None else rec_engine == winner)
    soundness = "OK" if (agree or not (ns and cs)) else "MISMATCH"

    _say(f"  dive={rec}(->{rec_engine})  winner={winner} spread={spread}s  "
         f"dive_correct={dive_correct}  counts={soundness}")
    rec_json = {"ts": _now(), "inst": name, "native": native, "canonical": canon,
                "dive": rec, "winner": winner, "spread_s": spread,
                "dive_correct": dive_correct, "soundness": soundness,
                "merge_ratio": dive["signals"]["merge_ratio"],
                "cost_ratio": dive["signals"]["cost_ratio"]}
    jf.write(json.dumps(rec_json, default=str) + "\n"); jf.flush()
    rows.append((name, rec_engine, winner, spread, dive_correct, soundness))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("cnfs", nargs="+")
    ap.add_argument("--budget", type=float, default=240.0)
    ap.add_argument("--warm", type=int, default=3)
    ap.add_argument("--maxcache", default="26000")
    ap.add_argument("--jsonl", default=os.path.join(_REPO, "hash_study_log.jsonl"))
    a = ap.parse_args(argv)
    if not os.environ.get("PORTFOLIO_SHARPSAT_BIN"):
        os.environ["PORTFOLIO_SHARPSAT_BIN"] = cocoa_bin()  # COCOA/cocoa/build/sharpSAT

    jf = open(a.jsonl, "a")
    _say(f"# hash-study {_now()} | budget={a.budget}s maxcache={a.maxcache}")
    rows = []
    for cnf in a.cnfs:
        name = os.path.basename(cnf).replace("mc2025_track1_", "t1_").replace(".cnf", "")
        _say(f"\n########## {name}  [{_now()}] ##########")
        try:
            _study_one(cnf, name, a, jf, rows)
        except Exception as e:  # noqa: BLE001 — one bad instance shouldn't kill the run
            _say(f"  ERROR: {type(e).__name__}: {e}")
    jf.close()

    _say("\n" + "=" * 72)
    _say(f"{'inst':<8}{'dive->':<11}{'winner':<11}{'spread':<9}{'correct':<9}sound")
    for n, re_, w, sp, dc, sd in rows:
        _say(f"{n:<8}{str(re_):<11}{str(w):<11}{str(sp):<9}{str(dc):<9}{sd}")
    _say("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
