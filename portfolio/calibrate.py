#!/usr/bin/env python3
"""Hash-mode calibration for the COCOA engine via Monte-Carlo dives.

The "which cache hash (identity vs canonical)" decision for a COCOA-routed
instance can't be read off the CNF statically (the discriminating signal —
isomorphism surplus during search — is dynamic). This runs the cheap
`-calibrateDive` estimator (Solver::diveSample) under EACH hash mode with a
SHARED seed (so the comparison is differential), parses `DIVE_STATS`, and
recommends a hash.

HARD RULE: this only ever selects a flag — it never produces or trusts a model
count (the solver emits no count in -calibrateDive mode). A calibration bug is
therefore benign: a suboptimal hash choice, never a wrong answer.

Usage:
  python3 portfolio/calibrate.py [--n-dives N] [--warm S] [--seed K] <cnf>
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import solvers  # noqa: E402

_REPO_ROOT = Path(__file__).resolve().parent.parent
_COCOA_SOLVER = "sharpsat-separator"  # registry key for the COCOA engine
_DEFAULT_BIN = _REPO_ROOT / "cocoa" / "build" / "sharpSAT"

# --- decision thresholds (v1 defaults; TUNE via the probe-vs-full-run study) ---
SURPLUS_MIN = 0.15   # min fraction of components canonical must MERGE to bother
SWHR_MIN    = 0.02   # min dive size-weighted-hit-rate gain (only meaningful on a PARTIAL warm)
COST_CAP    = 50.0   # if canonical's per-key cost is > this x identity's, surplus rarely pays
FULL_WARM_HR = 0.999 # both hit_rates ~1 => warm fully solved => swhr not discriminating

_DIVE_RE = re.compile(r"DIVE_STATS\s+(.*)")


def _parse_dive(stdout: str) -> dict | None:
    """Parse a `DIVE_STATS ...` line into a typed dict, or None if absent."""
    m = _DIVE_RE.search(stdout or "")
    if not m:
        return None
    kv = {}
    for tok in m.group(1).split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            kv[k] = v
    out: dict = {"mode": kv.get("mode")}
    for k in ("dives", "probes", "hits", "cache_entries"):
        out[k] = int(float(kv.get(k, 0)))
    for k in ("hit_rate", "size_weighted_hit_rate", "us_per_key"):
        out[k] = float(kv.get(k, 0.0))
    return out


def calibrate_one(hash_mode: str, cnf: str, n_dives: int, warm_s: int,
                  seed: int) -> dict | None:
    """Run warm+dive under one hash mode; return parsed DIVE_STATS or None."""
    res = solvers.run(
        _COCOA_SOLVER, hash_mode, cnf,
        extra_flags=["-calibrateDive", str(n_dives), "-calibrateSeed", str(seed)],
        time_budget_s=warm_s,
    )
    return _parse_dive(res.stdout)


def decide_hash(ident: dict | None, canon: dict | None) -> tuple[str, dict]:
    """Pick 'identity' or 'canonical' from the two modes' dive stats.

    Returns (recommended, signals). `signals` is logged for the flywheel so the
    thresholds above can be tuned against full-run outcomes.
    """
    if ident is None or canon is None:
        # Indeterminate (trivial instance / no DIVE_STATS): fall back to the
        # COCOA default (canonical), which is the sound, more-powerful mode.
        return "canonical", {"reason": "indeterminate (missing DIVE_STATS)",
                             "merge_ratio": None, "swhr_delta": None,
                             "cost_ratio": None, "full_warm": None}

    ie, ce = ident["cache_entries"], canon["cache_entries"]
    merge_ratio = (1.0 - ce / ie) if ie > 0 else 0.0          # surplus existence
    swhr_delta = canon["size_weighted_hit_rate"] - ident["size_weighted_hit_rate"]
    cost_ratio = (canon["us_per_key"] / ident["us_per_key"]) if ident["us_per_key"] > 0 else float("inf")
    full_warm = ident["hit_rate"] >= FULL_WARM_HR and canon["hit_rate"] >= FULL_WARM_HR

    sig = {"merge_ratio": round(merge_ratio, 4), "swhr_delta": round(swhr_delta, 4),
           "cost_ratio": round(cost_ratio, 3), "full_warm": full_warm}

    if merge_ratio < SURPLUS_MIN:
        sig["reason"] = f"no isomorphism surplus (merge_ratio={merge_ratio:.2f} < {SURPLUS_MIN}); identity is cheaper"
        return "identity", sig
    if cost_ratio > COST_CAP:
        sig["reason"] = f"surplus exists (merge_ratio={merge_ratio:.2f}) but per-key cost {cost_ratio:.1f}x > {COST_CAP}x; identity"
        return "identity", sig
    if (not full_warm) and swhr_delta < SWHR_MIN:
        sig["reason"] = f"surplus (merge_ratio={merge_ratio:.2f}) but no dive-coverage gain (swhr_delta={swhr_delta:.2f}); identity"
        return "identity", sig
    sig["reason"] = (f"surplus merge_ratio={merge_ratio:.2f}, cost {cost_ratio:.1f}x"
                     + (", warm fully solved" if full_warm else f", swhr_delta=+{swhr_delta:.2f}")
                     + "; canonical")
    return "canonical", sig


def calibrate_hash(cnf: str, n_dives: int = 2000, warm_s: int = 2,
                   seed: int = 12345) -> dict:
    """Run both modes (shared seed) and recommend a hash. Pure decision — no count."""
    ident = calibrate_one("identity", cnf, n_dives, warm_s, seed)
    canon = calibrate_one("canonical", cnf, n_dives, warm_s, seed)
    rec, sig = decide_hash(ident, canon)
    return {
        "ts": _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "cnf": cnf, "solver": _COCOA_SOLVER, "n_dives": n_dives, "warm_s": warm_s,
        "seed": seed, "recommended_hash": rec, "signals": sig,
        "identity": ident, "canonical": canon,
    }


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="COCOA hash-mode calibration via dives.")
    ap.add_argument("cnf")
    ap.add_argument("--n-dives", type=int, default=2000)
    ap.add_argument("--warm", type=int, default=2, help="warm-solve budget in seconds (-t)")
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--log", default=str(_REPO_ROOT / "portfolio" / "calibration_log.jsonl"))
    args = ap.parse_args(argv)

    if not os.environ.get("PORTFOLIO_SHARPSAT_BIN"):
        os.environ["PORTFOLIO_SHARPSAT_BIN"] = str(_DEFAULT_BIN)
    if not os.path.exists(os.environ["PORTFOLIO_SHARPSAT_BIN"]):
        print(f"ERROR: COCOA binary not found: {os.environ['PORTFOLIO_SHARPSAT_BIN']} "
              f"(build it: ./build.sh)", file=sys.stderr)
        return 2

    rec = calibrate_hash(args.cnf, n_dives=args.n_dives, warm_s=args.warm, seed=args.seed)

    try:
        with open(args.log, "a") as fh:
            fh.write(json.dumps(rec, default=str) + "\n")
    except OSError as e:
        print(f"WARNING: could not append calibration log: {e}", file=sys.stderr)

    s = rec["signals"]
    print(f"=== COCOA hash calibration: {os.path.basename(args.cnf)} ===")
    for m in ("identity", "canonical"):
        d = rec[m]
        if d:
            print(f"  {m:9s} hit_rate={d['hit_rate']:.3f} "
                  f"swhr={d['size_weighted_hit_rate']:.3f} "
                  f"us/key={d['us_per_key']:.2f} entries={d['cache_entries']}")
        else:
            print(f"  {m:9s} (no DIVE_STATS)")
    print(f"  signals: merge_ratio={s['merge_ratio']} swhr_delta={s['swhr_delta']} "
          f"cost_ratio={s['cost_ratio']} full_warm={s['full_warm']}")
    print(f"  --> recommend -hashMode {rec['recommended_hash']}   ({s['reason']})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
