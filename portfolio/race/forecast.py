"""Round-2 handoff forecaster — pure Python, ZERO external deps.

Decides COCOA-round-3 vs Ganak after the frontrunner round, by predicting whether
the COCOA leader will FINISH within the remaining budget from its short progress
trajectory (~18 samples over ~180 s of pct_lin).

No numpy / scipy: the MC competition run environment has no network and we deploy
light (an OLS slope is ~10 lines of stdlib).

MODEL (constant-rate + jump bonus).  We replaced the old power-law-on-remaining-
bits, which assumed the early plateau decelerates forever and so reported ETA = inf
even for trajectories that are slowly-but-steadily progressing (the t1_013 / scaled-
t1_101 failures). Instead:

  1. RATE   estimate the rate s = d(pct_lin)/dt as a RECENCY-WEIGHTED (exponential,
            EWMA-style) OLS slope over ALL samples: every sample counts, but recent
            ones weigh more (w_i = alpha^age, most-recent = 1), so a recent plateau
            lowers the projected rate without a hard window cutoff -- the same idea
            as ARIMA's geometric lag decay.
  2. BASE   base_eta = (FINISH_PCT - pct_now) / s   -- time to reach FINISH_PCT at
            the recent constant rate. ALWAYS FINITE when s > 0.
  3. BONUS  B = beta * sum_i alpha^(k-i) * max((delta_i/mean_delta)^gamma - 1, 0),
            over the per-interval pct_lin gains delta_i (k intervals, mean baseline,
            NO startup exclusion). Recency-weighted (alpha), concave (gamma<1) so one
            giant spike can't dominate. Captures "this config makes big jumps".
  4. ETA    eta = base_eta / (1 + B)   -- the bonus can only SHRINK the ETA (B>=0).
  5. CALL   COCOA if eta <= MARGIN * t_remaining, else GANAK.

INFINITY INVARIANT: eta is infinite IFF the recent rate s <= 0 (genuinely flat /
regressing). Any positive progress -> finite ETA. This is the property the old model
violated and the property the tests pin (test_forecast.py).

All thresholds are CALIBRATION KNOBS to tune from logged (snapshot -> outcome) pairs.
"""
from __future__ import annotations

import math

# --- calibration knobs (tune over runs) ---
FINISH_PCT = 100.0   # project pct_lin to this completion level
MARGIN = 0.9         # keep COCOA if ETA <= MARGIN * remaining budget (10% overrun cushion)
MIN_SAMPLES = 3      # need >= this many trajectory samples to estimate a rate
RATE_ALPHA = 0.9     # rate-slope recency decay per sample (EWMA-style); recent -> 1
# jump-bonus knobs
ALPHA = 0.95         # recency decay: weight alpha^(k-i), most recent interval -> 1
GAMMA = 0.6          # concave power (<1) compresses big spikes (anti-dominance)
BETA = 1.0           # overall bonus scale; eta divided by (1+B), B=BETA*sum_b (uncalibrated)


def remaining_bits(pct_lin: float) -> float:
    """r = log2(100 / pct_lin), pct_lin in PERCENT. +inf for pct_lin <= 0.
    Kept as a pure helper for logging / callers; not used in the decision."""
    if pct_lin <= 0.0:
        return math.inf
    return math.log2(100.0 / pct_lin)


def progress_bonus(traj, alpha: float = ALPHA, gamma: float = GAMMA,
                   beta: float = BETA, include_open: bool = True) -> float:
    """Jump bonus B >= 0 from the per-interval pct_lin gains.

    delta_i = pct_lin gain in interval i (interval 1 = progress from 0 to the first
    sample, i.e. the opening burst is INCLUDED, no startup exclusion). With
    mean_delta = mean(delta_i):

        B = beta * sum_i alpha^(k-i) * max((delta_i / mean_delta)^gamma - 1, 0)

    Properties (pinned by tests): B >= 0; SCALE-INVARIANT (multiplying every delta by
    a constant leaves B unchanged, since delta_i/mean_delta is unchanged); recent
    peaks weigh more (alpha<1); gamma<1 stops one spike from dominating; a perfectly
    steady trajectory (all deltas equal) gives B = 0.
    """
    pts = sorted((float(t), float(p)) for t, p in traj)
    if len(pts) < 2:
        return 0.0
    if include_open:        # pct_lin: interval 1 = 0 -> first sample (opening burst)
        deltas = [max(pts[0][1], 0.0)]
        deltas += [max(pts[i][1] - pts[i - 1][1], 0.0) for i in range(1, len(pts))]
    else:                   # closed_bits: arbitrary baseline -> inter-sample deltas only
        deltas = [max(pts[i][1] - pts[i - 1][1], 0.0) for i in range(1, len(pts))]
    k = len(deltas)
    if k == 0:
        return 0.0
    mean = sum(deltas) / k
    if mean <= 0.0:
        return 0.0
    s = 0.0
    for idx, d in enumerate(deltas):
        w = alpha ** (k - 1 - idx)            # idx=k-1 (most recent) -> weight 1
        s += w * max((d / mean) ** gamma - 1.0, 0.0)
    return beta * s


def _recent_rate(pts, alpha: float = RATE_ALPHA) -> float:
    """pct_lin rate (%/s) as a RECENCY-WEIGHTED OLS slope over ALL samples.

    Standard exponentially-weighted least squares: weight w_i = alpha^age with the
    most-recent sample at weight 1 (EWMA-style; the geometric decay behind ARIMA's
    lag operators). Every sample contributes, but recent ones dominate -- so a recent
    plateau pulls the slope down without any hard window cutoff, and the slope is <= 0
    (=> inf ETA) only when there is no net recent progress."""
    n = len(pts)
    xs = [t for (t, _) in pts]
    ys = [p for (_, p) in pts]
    w = [alpha ** (n - 1 - i) for i in range(n)]
    wsum = sum(w)
    mx = sum(wi * x for wi, x in zip(w, xs)) / wsum
    my = sum(wi * y for wi, y in zip(w, ys)) / wsum
    sxx = sum(wi * (x - mx) ** 2 for wi, x in zip(w, xs))
    sxy = sum(wi * (x - mx) * (y - my) for wi, x, y in zip(w, xs, ys))
    return sxy / sxx if sxx > 0.0 else 0.0


def predict(traj, t_active: float, t_remaining: float,
            finish_pct: float = FINISH_PCT, margin: float = MARGIN,
            alpha: float = ALPHA, gamma: float = GAMMA, beta: float = BETA,
            rate_alpha: float = RATE_ALPHA) -> dict:
    """traj: list of (t_seconds, pct_lin_percent) for the leader. t_active: leader's
    accumulated active seconds (kept for signature/logging compatibility). t_remaining:
    budget left for round 3.

    Returns a decision dict: engine in {"cocoa","ganak"}, a human `reason`, and the
    calibration fields (rate, base_eta_s, bonus_B, eta_s, proj_pct_lin_at_budget).
    """
    out = {"engine": "ganak", "reason": "", "eta_s": None, "base_eta_s": None,
           "rate_pct_per_s": None, "bonus_B": None, "current_pct_lin": None,
           "proj_pct_lin_at_budget": None, "n_samples": 0}

    pts = sorted((float(t), float(p)) for (t, p) in traj
                 if math.isfinite(t) and math.isfinite(p))
    out["n_samples"] = len(pts)
    if len(pts) < MIN_SAMPLES:
        out["reason"] = f"insufficient samples ({len(pts)} < {MIN_SAMPLES}) -> ganak"
        return out

    pct_now = pts[-1][1]
    out["current_pct_lin"] = pct_now

    # already done / essentially done -> finish it
    if pct_now >= finish_pct:
        out.update(engine="cocoa", eta_s=0.0, base_eta_s=0.0,
                   reason=f"already at pct_lin={pct_now:.2f}% >= {finish_pct}")
        return out

    s = _recent_rate(pts, rate_alpha)
    out["rate_pct_per_s"] = s
    B = progress_bonus(pts, alpha, gamma, beta)
    out["bonus_B"] = B

    # INFINITY INVARIANT: flat/regressing recent rate is the ONLY infinite ETA.
    if s <= 0.0:
        out["eta_s"] = math.inf
        out["reason"] = (f"recent pct_lin rate <= 0 ({s:.2e} %/s) -> flat/stuck "
                         f"(pct_lin={pct_now:.3f}%) -> ganak")
        return out

    base_eta = (finish_pct - pct_now) / s
    eta = base_eta / (1.0 + B)
    out["base_eta_s"] = base_eta
    out["eta_s"] = eta
    out["proj_pct_lin_at_budget"] = min(100.0, pct_now + s * t_remaining)

    tag = (f"base {base_eta:.0f}s /(1+B={B:.2f}), rate={s:.3g}%/s, "
           f"proj@budget={out['proj_pct_lin_at_budget']:.1f}%")
    if eta <= margin * t_remaining:
        out["engine"] = "cocoa"
        out["reason"] = f"ETA {eta:.0f}s <= {margin:.0%}x{t_remaining:.0f}s ({tag})"
    else:
        out["engine"] = "ganak"
        out["reason"] = f"ETA {eta:.0f}s > {margin:.0%}x{t_remaining:.0f}s ({tag})"
    return out


def predict_cb(traj, n_root: float, t_active: float, t_remaining: float,
               margin: float = MARGIN, alpha: float = ALPHA, gamma: float = GAMMA,
               beta: float = BETA, rate_alpha: float = RATE_ALPHA) -> dict:
    """Closed-bits time-series forecaster (better-conditioned alternative to predict).

    traj: list of (t_seconds, closed_bits) for the leader. n_root: target closed_bits
    where pct_lin = 100% (the solver emits it as `OPEN_WORK n_root`; offline it is
    recoverable as cb - log2(pct_lin%/100)). ETA = time for closed_bits to reach
    n_root at the recency-weighted closed_bits rate, shrunk by the jump bonus.

    Why closed_bits, not pct_lin: pct_lin = 2^(closed_bits - n_root), an EXPONENTIAL
    transform, so forecasting pct_lin squashes to ~0 and DEGENERATES on deep-tail
    instances (every config's rate -> 0 -> ETA = inf, no ranking; see t1_001). closed_bits
    is the linear, monotonic underlying signal -> non-degenerate, well-conditioned.
    Same INFINITY INVARIANT: eta is inf IFF the closed_bits rate <= 0 (genuinely stuck).
    The bonus runs on closed_bits deltas with include_open=False (the first sample is an
    arbitrary baseline, not a 0->burst), and is scale-invariant so BETA is unchanged.
    """
    out = {"engine": "ganak", "reason": "", "eta_s": None, "base_eta_s": None,
           "rate_bits_per_s": None, "bonus_B": None, "closed_bits": None,
           "n_root": n_root, "remaining_bits": None, "n_samples": 0}
    pts = sorted((float(t), float(cb)) for (t, cb) in traj
                 if math.isfinite(t) and math.isfinite(cb))
    warm = [(t, cb) for (t, cb) in pts if cb > 0.0]    # drop not-yet-warmed leading pts
    if warm:
        pts = warm
    out["n_samples"] = len(pts)
    if len(pts) < MIN_SAMPLES:
        out["reason"] = f"insufficient samples ({len(pts)} < {MIN_SAMPLES}) -> ganak"
        return out
    cb_now = pts[-1][1]
    out["closed_bits"] = cb_now
    remaining = n_root - cb_now
    out["remaining_bits"] = remaining
    if remaining <= 0.0:
        out.update(engine="cocoa", eta_s=0.0, base_eta_s=0.0,
                   reason=f"closed_bits={cb_now:.2f} >= n_root={n_root:.2f} (done)")
        return out
    s = _recent_rate(pts, rate_alpha)                  # bits/s, recency-weighted slope
    out["rate_bits_per_s"] = s
    B = progress_bonus(pts, alpha, gamma, beta, include_open=False)
    out["bonus_B"] = B
    if s <= 0.0:
        out["eta_s"] = math.inf
        out["reason"] = (f"closed_bits rate <= 0 ({s:.2e} b/s) -> stuck "
                         f"(cb={cb_now:.2f}/{n_root:.2f}) -> ganak")
        return out
    base_eta = remaining / s
    eta = base_eta / (1.0 + B)
    out["base_eta_s"] = base_eta
    out["eta_s"] = eta
    tag = f"rem {remaining:.2f}b /({s:.4g}b/s)/(1+B={B:.2f})"
    if eta <= margin * t_remaining:
        out.update(engine="cocoa",
                   reason=f"ETA {eta:.0f}s <= {margin:.0%}x{t_remaining:.0f}s ({tag})")
    else:
        out.update(engine="ganak",
                   reason=f"ETA {eta:.0f}s > {margin:.0%}x{t_remaining:.0f}s ({tag})")
    return out


# --- quick smoke self-test (python race/forecast.py); full suite in tests/ ---
def _linear(ts, rate, p0=0.0):
    return [(t, p0 + rate * t) for t in ts]


def _selftest() -> int:
    ts = list(range(10, 181, 10))
    TR = 2880.0
    cases = []

    # 1) steady SLOW progress -> FINITE eta (the inf regression), ganak (too slow)
    d = predict(_linear(ts, 0.03), 180, TR)   # ~5.4% by 180s
    cases.append(("slow-steady -> finite&ganak", math.isfinite(d["eta_s"]) and d["engine"] == "ganak", d))
    # 2) truly flat -> inf -> ganak  (the only legitimate infinity)
    d = predict([(t, 0.3) for t in ts], 180, TR)
    cases.append(("flat -> inf&ganak", d["eta_s"] == math.inf and d["engine"] == "ganak", d))
    # 3) fast finisher -> cocoa, finite
    d = predict(_linear(ts, 0.5), 180, TR)     # ~90% by 180s
    cases.append(("fast -> cocoa", d["engine"] == "cocoa" and math.isfinite(d["eta_s"]), d))
    # 4) too few samples -> ganak
    d = predict([(180, 5.0)], 180, TR)
    cases.append(("fewpts -> ganak", d["engine"] == "ganak", d))

    n = 0
    for name, ok, d in cases:
        n += ok
        es = "inf" if d["eta_s"] == math.inf else (f"{d['eta_s']:.0f}s" if d["eta_s"] is not None else "None")
        print(f"  {name:32s} PASS={ok}  [{d['engine']}, eta={es}]")
    print(f"\n===== forecast smoke-test: {n}/{len(cases)} PASS (full suite: pytest tests/test_forecast.py) =====")
    return 0 if n == len(cases) else 1


if __name__ == "__main__":
    import sys
    sys.exit(_selftest())
