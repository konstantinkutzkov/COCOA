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


# ===========================================================================
# Recency-weighted progress-rate forecaster on closed_bits (REAL SECONDS).
#
#   eta = remaining_bits / rate
#   rate = Sum_i w_i * r_i / Sum_i w_i        (recency-weighted mean interval rate)
#   r_i  = (cb_i - cb_{i-1}) / (t_i - t_{i-1})            bits/sec for interval i
#   w_i  = (1 + age_i / TAU) ^ (-POWER)        POWER-LAW recency weight,
#          age_i = t_now - midpoint(interval i)           in SECONDS
#
# The power-law tail was chosen empirically (sweep over t1_045/mc2026_007 [must
# KEEP] vs mc2026_011/t1_017/t1_019/t1_031 [must BAIL]) to satisfy two opposing
# goals at once:
#   * sharp drop near age 0   -> a fresh wall dominates the average fast, so a
#     genuinely-stalled config is handed to Ganak promptly (objective: fast bail);
#   * heavy tail at large age  -> a strong early burst is NOT forgotten, so a
#     config slowly creeping to a finish (t1_045) never drifts near a handoff
#     (objective: a recoverer is never endangered).
# It replaces the ARIMA(1,1,0)+drift model, whose constant-rate assumption was
# structurally over-optimistic on decelerating instances (it never saw the wall).
# Time-indexed in seconds => invariant to the sampling interval.
#
# PROVISIONAL WEIGHTS: POWER=2 and TAU=120s are the current best fit on only ~6
# instances (just ONE true recoverer, t1_045). They are TUNING KNOBS, not derived
# constants -- expect to re-fit both as more progress traces accumulate. (e.g.
# exp tau=240 was nearly as good; revisit once the keep/bail corpus grows.)
#
# The BAIL decision is NOT here: the scheduler hands to Ganak only after
# R3_CONSECUTIVE (=10) consecutive rechecks forecast eta > overshoot x budget.
# ===========================================================================
RWR_MIN_SAMPLES = 4        # need a few intervals before forecasting
RWR_MIN_SPAN_S  = 90.0     # scheduler gate: prefer this forecaster over predict_cb once a
                           # config has >= this many SECONDS of trajectory (round-2+ / monitor)
RWR_TAU         = 120.0    # PROVISIONAL recency time-constant (seconds) -- retune with data
RWR_POWER       = 2.0      # PROVISIONAL power-law tail exponent     -- retune with data
RWR_RATE_EPS    = 1e-9     # recency-weighted rate at/below this (b/s) => stuck => inf ETA


def predict_cb_recency(traj, n_root, t_active, t_remaining, margin=MARGIN,
                       tau=RWR_TAU, power=RWR_POWER):
    """Recency-weighted progress-rate ETA on closed_bits. eta = remaining / rate, where
    rate is a power-law recency-weighted mean of the per-interval cb rate (see header).
    Same return shape as predict_cb. eta_s is inf IFF the recency-weighted rate <= 0
    (no recent net progress => never reaches n_root). tau/power are provisional knobs."""
    out = {"engine": "ganak", "reason": "", "eta_s": None, "rate_bits_per_s": None,
           "closed_bits": None, "n_root": n_root, "remaining_bits": None,
           "n_samples": 0, "stuck": False, "tau": tau, "power": power}
    pts = sorted((float(t), float(cb)) for (t, cb) in traj
                 if math.isfinite(t) and math.isfinite(cb))
    warm = [(t, cb) for (t, cb) in pts if cb > 0.0]    # drop not-yet-warmed leading pts
    if warm:
        pts = warm
    n = len(pts)
    out["n_samples"] = n
    if n < RWR_MIN_SAMPLES:
        out["reason"] = f"insufficient samples ({n} < {RWR_MIN_SAMPLES}) -> ganak"
        return out
    t_now, cb_now = pts[-1]
    out["closed_bits"] = cb_now
    remaining = n_root - cb_now
    out["remaining_bits"] = remaining
    if remaining <= 0.0:
        out.update(engine="cocoa", eta_s=0.0,
                   reason=f"cb {cb_now:.2f} >= n_root {n_root:.2f} (done)")
        return out

    # recency-weighted mean of per-interval rates; POWER-LAW tail w=(1+age/tau)^(-power)
    num = den = 0.0
    for i in range(1, n):
        (ta, ca), (tb, cbi) = pts[i - 1], pts[i]
        dt = tb - ta
        if dt <= 0.0:
            continue
        r = (cbi - ca) / dt                          # bits/sec over interval i
        age = t_now - 0.5 * (ta + tb)                # seconds before now (midpoint)
        w = (1.0 + age / tau) ** (-power)
        num += w * r
        den += w
    rate = num / den if den > 0.0 else 0.0
    out["rate_bits_per_s"] = rate

    # the ONLY infinite ETA: no recent net progress -> never reaches n_root
    if rate <= RWR_RATE_EPS:
        out.update(eta_s=math.inf, stuck=True,
                   reason=f"recency rate {rate:.2e} b/s <= 0 -> stuck -> inf -> ganak")
        return out

    eta = remaining / rate
    out["eta_s"] = eta
    tag = f"rem {remaining:.2f}b / rate {rate:.4g}b/s (pow={power:.0f}, tau={tau:.0f}s)"
    if eta <= margin * t_remaining:
        out.update(engine="cocoa", reason=f"ETA {eta:.0f}s <= {margin:.0%}x{t_remaining:.0f}s ({tag})")
    else:
        out.update(engine="ganak", reason=f"ETA {eta:.0f}s > {margin:.0%}x{t_remaining:.0f}s ({tag})")
    return out


# ===========================================================================
# Escape-history decision-tree handoff forecaster (predict_cb_tree).
#
# Per-recheck KEEP/BAIL decision for the Ganak handoff (NOT a ranker). Replaces
# the recency-rate "eta > budget" test, which had two failures no single rate
# could avoid together: false-bailing a config that recovers from multi-minute
# plateaus (t1_045), and a single early jump pinning eta near 0 so a walled config
# never bailed (017 / the "hostage" cases). The discriminator is the PLATEAU-ESCAPE
# HISTORY -- not "made a big jump": t1_045 escaped plateaus of ~1.2/0.3/3min
# repeatedly; 017 made one cliff jump then never escaped a plateau -> bail. Uses
# closed_bits ONLY (a `decisions` signal misleads: 017 had decisions growing yet
# was doomed). The scheduler fires the real handoff only after R3_CONSECUTIVE
# 'bail' verdicts in a row AND >= R3_MIN_ACTIVE_S active (debounce unchanged).
#
# Designed via a 3-variant panel + adversarial verify against the 7-case keep/bail
# harness (portfolio/_esc_harness.py): KEEP {007, t1_045}; BAIL {011, 017, t1_019,
# t1_031, hostage} -- all 7 correct, plus edge cases (n_root=inf/None, <4 samples,
# irregular dt, first-move-non-escape recoverer). Conservative GRACE=120 / K=3
# favor recoverer-safety over raw bail speed; thresholds are tunable toward faster
# bail WITHOUT touching the escape detector. RESIDUAL RISKS (benchmark_log): the
# ETA budget-guard is the only non-pure-cb extrapolation (gated behind
# beyond-envelope, never load-bearing on the 7 cases); STALL_RATE/PLAT_RATE share
# one gate, so an ultra-slow-but-real grinder could read as a single plateau.
# ===========================================================================
def predict_cb_tree(traj, n_root, t_active, t_remaining):
    """Escape-history Ganak-handoff forecaster (conservative variant). Returns
    {'verdict': 'keep'|'bail', 'reason': str}. An *escape* is a cb jump (>= jump_thr)
    immediately preceded by a real plateau (>= MIN_PLAT s of near-zero progress); a
    lone opening jump (from 0 or a positive baseline) has no preceding plateau and is
    NOT an escape. 0 escapes ever => never demonstrated it can break a plateau => bail
    on the current stall. Else patience = K * max(escape_gap)."""
    # ---- edge: insufficient data ----
    if traj is None or len(traj) < 4:
        return {'verdict': 'keep', 'reason': 'insufficient samples (<4)'}

    t_end, cb_now = traj[-1][0], traj[-1][1]
    finite_root = (n_root is not None) and math.isfinite(n_root)
    remaining = (n_root - cb_now) if finite_root else float('inf')

    # ---- complete: nothing left to close ----
    if finite_root and remaining <= 0:
        return {'verdict': 'keep', 'reason': 'remaining<=0 (count complete)'}

    # ---- per-instance scale for a "meaningful jump" ----
    cb_first = traj[0][1]
    span = max(cb_now - cb_first, 1e-9)            # total progress achieved so far
    JUMP_FRAC = 0.01                              # >= 1% of total progress, or ...
    JUMP_ABS = 0.10                               # ... >= 0.10 bits absolute floor
    jump_thr = max(JUMP_ABS, JUMP_FRAC * span)

    # ---- recent_rate over a trailing window (conservative: long 180s window) ----
    RECENT_WIN = 180.0
    EPS = 2e-5                                     # bits/sec; below this == "not progressing"
    j = len(traj) - 1
    while j > 0 and (t_end - traj[j][0]) < RECENT_WIN:
        j -= 1
    win_dt = max(t_end - traj[j][0], 1e-9)
    recent_rate = (cb_now - traj[j][1]) / win_dt
    if recent_rate > EPS:
        # Progressing -- but FAST ENOUGH to finish in the remaining budget? A leader can
        # creep at recent_rate>eps yet need many times the budget to close `remaining`
        # bits. mc2026_027 rode such a "progressing but doomed" leader for ~16 min of
        # active time (eta ~4500s vs ~2000s budget) because this branch only asked
        # "moving?", never "moving fast enough?" -- the budget-aware eta check the old
        # recency forecaster had was dropped here. Restore it: bail a progressing leader
        # whose eta blows the budget -- UNLESS it is a near-finisher
        # (remaining <= KEEP_REM_FLOOR, e.g. 025 at 91%), which we always ride out.
        KEEP_ETA_MARGIN = 2.0      # keep iff eta <= 2x the remaining budget
        KEEP_REM_FLOOR = 2.0       # bits; never bail a leader this close to done
        if finite_root and t_remaining > 0 and remaining > KEEP_REM_FLOOR:
            eta = remaining / recent_rate
            if eta > KEEP_ETA_MARGIN * t_remaining:
                return {'verdict': 'bail',
                        'reason': (f'progressing but doomed: eta={eta:.0f}s>'
                                   f'{KEEP_ETA_MARGIN:.0f}x{t_remaining:.0f}s '
                                   f'(rem={remaining:.1f} rate={recent_rate:.6f})')}
        return {'verdict': 'keep', 'reason': f'progressing recent_rate={recent_rate:.6f}>eps'}

    # ---- stall_dur: seconds since the last *meaningful* cb increase ----
    avg_rate = span / max(t_end - traj[0][0], 1e-9)
    STALL_RATE = max(5e-4, 0.02 * avg_rate)       # bits/sec gate for "real" progress
    last_inc_t = traj[0][0]
    for i in range(1, len(traj)):
        dt = traj[i][0] - traj[i - 1][0]
        dt = dt if dt > 0 else 10.0
        if (traj[i][1] - traj[i - 1][1]) / dt >= STALL_RATE:
            last_inc_t = traj[i][0]
    stall_dur = t_end - last_inc_t

    # ---- grace: short plateaus get a fair chance (conservative: generous) ----
    GRACE = 120.0
    if stall_dur < GRACE:
        return {'verdict': 'keep', 'reason': f'short plateau stall={stall_dur:.0f}<grace'}

    # ---- escapes: jumps PRECEDED BY A PLATEAU span (>= MIN_PLAT seconds) ----
    MIN_PLAT = 20.0
    PLAT_RATE = STALL_RATE                         # same "near-zero" gate
    escape_gaps = []
    plat_start = None
    prev_t, prev_c = traj[0]
    for i in range(1, len(traj)):
        t, c = traj[i]
        dt = t - prev_t if t > prev_t else 10.0
        dc = c - prev_c
        if dc >= jump_thr:                         # a jump
            if plat_start is not None:
                gap = prev_t - plat_start
                if gap >= MIN_PLAT:                # only counts if a real plateau preceded it
                    escape_gaps.append(gap)
            plat_start = None
        elif (dc / dt) < PLAT_RATE:                # plateau step
            if plat_start is None:
                plat_start = prev_t
        else:                                      # small but real progress (not an escape)
            plat_start = None
        prev_t, prev_c = t, c

    # ---- never escaped a plateau -> doomed (017 cliff-from-0 / hostage cliff-from-baseline) ----
    if not escape_gaps:
        return {'verdict': 'bail', 'reason': 'no escapes ever (never escaped a plateau)'}

    # ---- within demonstrated escape envelope -> fair chance (conservative K=3) ----
    K = 3.0
    max_gap = max(escape_gaps)
    if stall_dur < K * max_gap:
        return {'verdict': 'keep', 'reason': f'within escape envelope stall={stall_dur:.0f}<{K}*{max_gap:.0f}'}

    # ---- stalled beyond the escape envelope -> bail. Budget guard is only a terminal
    #      backstop (envelope already exceeded); it never overrides a within-envelope keep. ----
    if not finite_root:
        return {'verdict': 'bail', 'reason': 'stalled beyond envelope & n_root not finite'}
    eta = (remaining / avg_rate) if avg_rate > 0 else float('inf')
    if eta > t_remaining:
        return {'verdict': 'bail', 'reason': f'beyond envelope & eta={eta:.0f}>t_rem'}
    return {'verdict': 'bail', 'reason': f'stalled {stall_dur:.0f}s beyond escape envelope'}


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
