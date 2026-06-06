#!/usr/bin/env python3
"""Backtest the ETA-funnel proposal with the closed_bits time-series forecaster.

predict_cb forecasts closed_bits -> n_root (well-conditioned; non-degenerate on
deep-tail instances where pct_lin flatlines). Checks:
  (A) SELECTION at round-1: rank configs by predict_cb ETA; keep the winner? and is
      it RANKABLE on t1_001, where the old pct_lin predict() was all-inf?
  (B) HANDOFF: winner monitoring trajectory through "ETA > 1.25x rem, sustained >=100s".
"""
import json, re, sys, math, os
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from race.forecast import predict, predict_cb

BUDGET = 3600.0
RUNLOGS = os.path.join(HERE, "runlogs")
TRAJLOG = os.path.join(HERE, "trajectory_log.jsonl")
MON_START, HANDOFF_X, HANDOFF_SUSTAIN = 180.0, 1.25, 100.0
INSTANCES = {
    "t1_045": {"src": "runlog",  "winner": "cocoa-adaptive-nosep", "bound": 60},
    "t1_089": {"src": "runlog",  "winner": "cocoa-nosep-cascade",  "bound": 60},
    "t1_049": {"src": "trajlog", "winner": "cocoa-plain",          "bound": 50},
    "t1_001": {"src": "trajlog", "winner": None,                   "bound": 80},
}
live_re = re.compile(r'\[r\d~live\] (cocoa-[a-z-]+): pct_lin=([0-9.eE+-]+) '
                     r'closed_bits=([0-9.eE+-]+) decisions=\d+ \(active (\d+)s\)')
summ_re = re.compile(r'\[r\d\] (cocoa-[a-z-]+): pct_lin=([0-9.eE+-]+) '
                     r'closed_bits=([0-9.eE+-]+) decisions=\d+ closed_bits=[0-9.]+ '
                     r'vel=[0-9.eE+-]+ \(active (\d+)s\)')

def parse_runlog(inst):
    cfg = {}
    for line in open(os.path.join(RUNLOGS, f"{inst}.log")):
        for rx in (live_re, summ_re):
            m = rx.search(line)
            if m:
                c, pct, cb, t = m.group(1), float(m.group(2)), float(m.group(3)), int(m.group(4))
                cfg.setdefault(c, {})[t] = (t, pct, cb); break
    return {c: sorted(d.values()) for c, d in cfg.items()}

def load_trajlog_cfg(inst):
    key = "track1_" + inst[3:]; cfg = {}
    for line in open(TRAJLOG):
        d = json.loads(line)
        if key in d["cnf"] and str(d.get("config", "")).startswith("cocoa-"):
            cfg[d["config"]] = sorted((float(p["t"]), float(p["pct_lin"]), float(p.get("closed_bits", 0)))
                                      for p in d["trajectory"])
    return cfg

def recover_n_root(points):
    # n_root = cb - log2(pct%/100) at the MAX-pct point (least relative noise in log2 pct).
    best = max(((pct, cb) for (t, pct, cb) in points if pct > 0), default=None)
    return None if best is None else best[1] - math.log2(best[0] / 100.0)

def fmt(e): return "inf" if (e is None or e == math.inf) else (f"{e:.0f}" if e < 1e7 else f"{e:.1e}")

print("=" * 80)
print("(A) ROUND-1 SELECTION via predict_cb (closed_bits time-series -> ETA)")
print("=" * 80)
for inst, meta in INSTANCES.items():
    cfg = parse_runlog(inst) if meta["src"] == "runlog" else load_trajlog_cfg(inst)
    n_root = recover_n_root([p for pts in cfg.values() for p in pts])
    bound, winner = meta["bound"], meta["winner"]
    rows = []
    for c, pts in cfg.items():
        sub = [(t, cb) for (t, pct, cb) in pts if t <= bound]
        if len(sub) < 3: continue
        last = [x for x in pts if x[0] <= bound][-1]
        d = predict_cb(sub, n_root, sub[-1][0], BUDGET - sub[-1][0])
        do = predict([(t, pct) for (t, pct, cb) in pts if t <= bound], sub[-1][0], BUDGET - sub[-1][0])
        rows.append({"c": c, "pct": last[1], "cb": last[2], "rate": d["rate_bits_per_s"],
                     "eta": d["eta_s"], "old": do["eta_s"]})
    ekey = lambda r: (r["eta"] if (r["eta"] is not None and r["eta"] != math.inf) else float("inf"))
    by_eta = sorted(rows, key=ekey); by_cb = sorted(rows, key=lambda r: -r["cb"])
    n = len(rows); keep = min(4, n - 1)
    print(f"\n----- {inst}  (n={n}, bound={bound}s, n_root~{n_root:.1f}, winner={winner}) -----")
    print(f"  {'config':22}{'pct_lin':>11}{'cb':>9}{'cb_rate':>9}{'eta_cb':>9}{'eta/rem':>8}{'OLD_eta':>11}")
    for r in by_eta:
        er = (r["eta"] / (BUDGET - bound)) if (r["eta"] and r["eta"] != math.inf) else math.inf
        print(f"  {r['c']:22}{r['pct']:11.4g}{r['cb']:9.2f}{(r['rate'] or 0):9.4f}"
              f"{fmt(r['eta']):>9}{('inf' if er==math.inf else f'{er:.2f}'):>8}{fmt(r['old']):>11}")
    top_eta = [r["c"] for r in by_eta[:keep]]; top_cb = [r["c"] for r in by_cb[:keep]]
    finite = sum(1 for r in rows if r["eta"] and r["eta"] != math.inf)
    finite_old = sum(1 for r in rows if r["old"] and r["old"] != math.inf and r["old"] < 1e7)
    print(f"  keep-{keep} by eta_cb     : {top_eta}")
    print(f"  rankable ETAs: predict_cb {finite}/{n} sane  vs  OLD pct_lin {finite_old}/{n} sane")
    if winner:
        re_ = [r["c"] for r in by_eta]; rc_ = [r["c"] for r in by_cb]
        print(f"  >> winner '{winner}': eta_cb rank #{re_.index(winner)+1} "
              f"({'kept' if winner in top_eta else 'CUT'}), cb rank #{rc_.index(winner)+1}")

print("\n" + "=" * 80)
print(f"(B) HANDOFF via predict_cb @ t>={MON_START:.0f}s: fire when ETA>{HANDOFF_X}x rem for >={HANDOFF_SUSTAIN:.0f}s")
print("=" * 80)
wtraj = {}
for line in open(TRAJLOG):
    d = json.loads(line)
    for inst in ("t1_045", "t1_089"):
        if f"track1_{inst[3:]}" in d["cnf"] and len(d.get("trajectory", [])) > 10:
            wtraj[inst] = d
for inst in ("t1_045", "t1_089"):
    if inst not in wtraj:
        print(f"  {inst}: no rich winner trajectory"); continue
    pp = [(float(p["t"]), float(p["pct_lin"]), float(p.get("closed_bits", 0))) for p in wtraj[inst]["trajectory"]]
    n_root = recover_n_root(pp); fin = wtraj[inst].get("finished")
    start = None; mx = 0.0; fire = None
    for i in range(len(pp)):
        t = pp[i][0]; t_rem = BUDGET - t
        if t < MON_START or t_rem <= 0 or i + 1 < 3: continue
        eta = predict_cb([(x[0], x[2]) for x in pp[:i + 1]], n_root, t, t_rem)["eta_s"]
        if (eta == math.inf) or (eta > HANDOFF_X * t_rem):
            start = t if start is None else start
            mx = max(mx, t - start)
            if (t - start) >= HANDOFF_SUSTAIN and fire is None: fire = t
        else:
            start = None
    res = f"FIRES at active {fire:.0f}s -> Ganak" if fire else f"never fires (longest overshoot {mx:.0f}s)"
    ok = "OK" if ((fire is not None) == (not fin)) else "*** WRONG ***"
    print(f"  {inst} (finished={fin}, n_root~{n_root:.0f}): {res}  {ok}")
