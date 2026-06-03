"""Per-config progress TRAJECTORY tool.

Runs each COCOA archetype on one instance and logs pct_lin / closed_bits /
decisions every `interval` seconds, so we can see the SHAPE of progress
(climbing / plateauing / flat) per config and infer observations. Streams live
(run with `python -u`) and appends a durable JSONL.

Usage: python -u race/trajectory.py <cnf> [--interval 10] [--budget 120]
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from race.archetypes import STRONG                        # noqa: E402

_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _say(m):
    print(m, flush=True)


def _kv(line):
    d = {}
    for tok in line.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            d[k] = v
    return d


def trajectory(arch, cnf, interval, budget, jf):
    env = dict(os.environ)
    env["SHARPSAT_PROGRESS"] = "1"
    env["SHARPSAT_PROGRESS_INTERVAL"] = str(interval)
    _say(f"\n=== {arch.name}   [{' '.join(arch.flags)}] ===")
    _say(f"  {'t(s)':>5} {'pct_lin':>14} {'closed_bits':>12} {'decisions':>13} {'l2_hits':>11}")
    # `timeout` bounds it; PROGRESS goes to stderr; we discard stdout.
    p = subprocess.Popen(["timeout", str(int(budget))] + arch.argv(cnf),
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                         text=True, bufsize=1, env=env)
    traj = []
    finished = False
    for line in p.stderr:
        if line.startswith("PROGRESS"):
            d = _kv(line)
            traj.append({k: d.get(k) for k in ("t", "pct_lin", "closed_bits", "decisions", "l2_hits")})
            _say(f"  {float(d.get('t', 0)):>5.0f} {d.get('pct_lin', '-'):>14} "
                 f"{d.get('closed_bits', '-'):>12} {d.get('decisions', '-'):>13} "
                 f"{d.get('l2_hits', '-'):>11}")
        elif "# solutions" in line or "exact arb" in line:
            finished = True
    p.wait()
    if finished:
        _say("  -> FINISHED within budget")
    rec = {"ts": _dt.datetime.now().replace(microsecond=0).isoformat(),
           "cnf": os.path.basename(cnf), "config": arch.name, "flags": list(arch.flags),
           "interval": interval, "budget": budget, "finished": finished, "trajectory": traj}
    jf.write(json.dumps(rec) + "\n"); jf.flush()
    return traj


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("cnf")
    ap.add_argument("--interval", type=float, default=10.0)
    ap.add_argument("--budget", type=float, default=120.0)
    ap.add_argument("--jsonl", default=os.path.join(_REPO, "trajectory_log.jsonl"))
    a = ap.parse_args(argv)
    cocoa = [x for x in STRONG if x.engine == "cocoa"]
    _say(f"# trajectory {os.path.basename(a.cnf)} | interval={a.interval}s "
         f"budget={a.budget}s | {len(cocoa)} COCOA configs")
    jf = open(a.jsonl, "a")
    for arch in cocoa:
        trajectory(arch, a.cnf, a.interval, a.budget, jf)
    jf.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
