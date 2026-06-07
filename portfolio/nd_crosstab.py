#!/usr/bin/env python3
"""Cross-tab: nd_cost signals x known winning separator strategy.

Validates the hypothesis that the nd_cost band predicts which COCOA separator
strategy wins (low->sep, mid->reactive, high->nosep/ganak). Joins
nd_calibrate.jsonl (nd signals) with a curated ground-truth strategy per
instance (mined from benchmark_log.md + memory + sweep_log.jsonl).
"""
from __future__ import annotations
import json, os

_HERE = os.path.dirname(os.path.abspath(__file__))

# Ground truth: instance -> (strategy, confidence, best_time/status, note)
# strategy: sep | reactive | nosep | ganak | unknown
GT = {
    "t1_001": ("ganak",    "high",   "timeout",     ""),
    "t1_003": ("sep",      "high",   "153s",        ""),
    "t1_005": ("unknown",  "low",    "-",           "untested"),
    "t1_007": ("ganak",    "high",   "timeout",     ""),
    "t1_011": ("sep",      "high",   "8.79s",       "FAST sep win despite huge cost"),
    "t1_013": ("nosep",    "medium", "timeout",     "nosep-cascade was leader"),
    "t1_021": ("sep",      "medium", "timeout",     ""),
    "t1_023": ("unknown",  "low",    "timeout60",   "all configs timed out @60s"),
    "t1_025": ("sep",      "high",   "7.31s",       ""),
    "t1_027": ("sep",      "high",   "4.80s",       ""),
    "t1_041": ("reactive", "high",   "3.04s",       "canonical mid->reactive"),
    "t1_045": ("sep",      "high",   "2065s",       "adaptive + -sep 5"),
    "t1_047": ("nosep",    "medium", "648s",        "dense-small, adaptive nosep"),
    "t1_049": ("sep",      "high",   "329s",        "beats ganak; big root_sep"),
    "t1_053": ("unknown",  "low",    "-",           "untested"),
    "t1_059": ("nosep",    "medium", "timeout",     "binary-heavy"),
    "t1_065": ("sep",      "high",   "0.016s",      ""),
    "t1_071": ("sep",      "high",   "0.47s",       ""),
    "t1_073": ("sep",      "high",   "0.336s",      ""),
    "t1_105": ("reactive", "low",    "partial",     "only partial measurements"),
    "t1_149": ("ganak",    "low",    "-",           "calibration only"),
    "t1_159": ("ganak",    "low",    "-",           "calibration only"),
    "t1_163": ("ganak",    "low",    "-",           "calibration only"),
}


def band(cost):
    if not isinstance(cost, (int, float)): return "?"
    if cost <= 40: return "low"
    if cost <= 90: return "mid"
    return "high"


def predicted_strategy(b):
    return {"low": "sep", "mid": "reactive", "high": "nosep/ganak"}.get(b, "?")


def main():
    rows = []
    with open(os.path.join(_HERE, "nd_calibrate.jsonl")) as fh:
        for line in fh:
            d = json.loads(line)
            inst = d.get("inst")
            gt = GT.get(inst, ("unknown", "low", "-", ""))
            rows.append((d, gt))
    rows.sort(key=lambda r: r[0].get("nd_log2_cost", 1e18))

    print(f"{'inst':7} {'cost':>6} {'mxpath':>6} {'rootsep':>7} {'wleaf':>5} "
          f"{'band':>4} {'pred':>11} | {'ACTUAL':>8} {'conf':>6} {'time':>9}  match  note")
    n_match = n_eval = 0
    for d, (strat, conf, t, note) in rows:
        c = d.get("nd_log2_cost"); b = band(c)
        pred = predicted_strategy(b)
        # match = actual strategy consistent with predicted band
        ok = "-"
        if strat in ("sep", "reactive", "nosep"):
            n_eval += 1
            hit = ((b == "low" and strat == "sep") or
                   (b == "mid" and strat == "reactive") or
                   (b == "high" and strat == "nosep"))
            ok = "YES" if hit else "no"
            if hit: n_match += 1
        cs = f"{c:.1f}" if isinstance(c, (int, float)) else str(c)
        print(f"{d.get('inst',''):7} {cs:>6} {str(d.get('nd_max_path_sep','')):>6} "
              f"{str(d.get('nd_root_sep_vars','')):>7} {str(d.get('nd_worst_leaf_vars','')):>5} "
              f"{b:>4} {pred:>11} | {strat:>8} {conf:>6} {t:>9}  {ok:>4}  {note}")
    print(f"\nband->strategy hypothesis: {n_match}/{n_eval} known-strategy instances match")


if __name__ == "__main__":
    main()
