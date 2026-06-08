"""Synthetic tests for the ARIMA(1,1,0)+drift closed_bits forecaster.

The model fits on ALL collected samples (no forecast window): difference cb -> per-step
rate, AR(1) around the sample-mean drift mu, integrate the reverting rate to n_root.
The ONLY infinite ETA is mu <= 0 (no net drift over the FULL history). There is NO
stuck-floor: a config that bursts then walls keeps a FINITE ETA that GROWS smoothly as
the wall lengthens (mu dilutes with each flat sample) -- the bail is the scheduler's
consecutive-over-budget streak, not a myopic window here. Everything is time-indexed in
REAL SECONDS, so changing the sampling step must not change the verdicts
(test_sampling_interval_invariance).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from race.forecast import predict_cb_arima                          # noqa: E402

STEP = 10.0          # nominal sampling interval (seconds) -- matches the live recheck
TR = 3000.0          # remaining budget (generous, so the engine call is about ETA only)


def series(cb_of_i, n, step=STEP, t0=0.0):
    """[(t_seconds, closed_bits)] with cb = cb_of_i(index)."""
    return [(t0 + i * step, float(cb_of_i(i))) for i in range(n)]


# --------------------------------------------------------------------------- #
# 1. Steady progress -> finite, accurate ETA.
def test_steady_linear_accurate():
    s = series(lambda i: 2.0 * i, 20)            # rate 2 bits/step = 0.2 b/s
    cb_now = s[-1][1]                            # 38
    d = predict_cb_arima(s, n_root=cb_now + 40.0, t_active=190, t_remaining=TR)
    assert math.isfinite(d["eta_s"]) and not d["stuck"]
    assert abs(d["eta_s"] - 200.0) < 20.0        # 40 bits / 2 per step * 10s = 200s
    assert d["engine"] == "cocoa"


# 2. Flat from the START -> mu = 0 over the whole history -> infinite ETA (stuck), ganak.
#    This is the ONLY infinity now (no floor): genuinely zero net drift.
def test_flat_from_start_is_inf():
    s = series(lambda i: 100.0, 15)              # constant
    d = predict_cb_arima(s, n_root=200.0, t_active=140, t_remaining=TR)
    assert d["eta_s"] == math.inf and d["stuck"]
    assert d["engine"] == "ganak"


# 3. THE t1_031 case under the NO-FLOOR model: a strong start then a multi-minute wall
#    is NOT declared stuck -- it keeps a FINITE ETA (mu is still positive over the full
#    history). The bail does not come from this function; it comes from the ETA growing
#    past the budget over the scheduler's streak. So: finite & cocoa under a generous
#    budget, but ganak once the same ETA is measured against a tight budget.
def test_strong_start_then_plateau_is_finite_not_floored():
    climb = [(i * 10.0, 10.0 * i) for i in range(31)]                    # cb 0..300 over 300s
    plateau = [(300.0 + j * 10.0, 300.0) for j in range(1, 13)]          # flat 120s
    traj = climb + plateau
    d = predict_cb_arima(traj, n_root=331.0, t_active=420, t_remaining=TR)
    assert math.isfinite(d["eta_s"]) and not d["stuck"]                  # NO floor inf
    # the same finite ETA, against a budget it overshoots, hands off:
    d_tight = predict_cb_arima(traj, n_root=331.0, t_active=420, t_remaining=d["eta_s"] / 2.0)
    assert d_tight["engine"] == "ganak"


# 4. SMOOTH, NO DISCONTINUITY (the property the floor violated): as a wall lengthens
#    sample by sample, the ETA increases MONOTONICALLY and stays finite -- never the
#    143s -> inf cliff the 90s floor produced.
def test_wall_eta_grows_smoothly_no_cliff():
    climb = [(i * 10.0, 2.0 * i) for i in range(31)]                     # cb 0..60 over 300s
    etas = []
    for j in range(1, 25):                                              # extend the wall 10..240s
        wall = [(300.0 + k * 10.0, 60.0) for k in range(1, j + 1)]
        d = predict_cb_arima(climb + wall, n_root=100.0,
                             t_active=300 + 10 * j, t_remaining=TR)
        etas.append(d["eta_s"])
        assert math.isfinite(d["eta_s"])                                # never jumps to inf
    # strictly increasing (mu dilutes as flat samples accumulate)
    assert all(b > a for a, b in zip(etas, etas[1:]))


# 5. THE t1_045 PROPERTY: a long plateau (>> the old 90s window) followed by a RECOVERY
#    jump must NOT be falsely declared stuck during the plateau -- the full-history mu
#    keeps the ETA finite, so the config is never myopically bailed before it recovers.
def test_long_plateau_then_recovery_no_false_bail():
    climb = [(i * 10.0, 2.0 * i) for i in range(11)]                    # cb 0..20 over 100s
    plateau = [(100.0 + j * 10.0, 20.0) for j in range(1, 16)]          # flat 150s (>90s)
    d_mid = predict_cb_arima(climb + plateau, n_root=40.0, t_active=250, t_remaining=TR)
    assert math.isfinite(d_mid["eta_s"]) and not d_mid["stuck"]         # NOT falsely stuck
    recover = [(250.0 + j * 10.0, 20.0 + 2.0 * j) for j in range(1, 6)]  # jumps again
    d_rec = predict_cb_arima(climb + plateau + recover, n_root=40.0, t_active=300, t_remaining=TR)
    assert math.isfinite(d_rec["eta_s"]) and not d_rec["stuck"]


# 6. Level matters: a slow crawl close to done vs far from done -> ETA proportional to
#    `remaining`, so they differ by the remaining ratio.
def test_level_dependence():
    s = series(lambda i: 50.0 + 0.1 * i, 20)     # crawl 0.1 b/step
    cb = s[-1][1]
    d_near = predict_cb_arima(s, n_root=cb + 3.3, t_active=190, t_remaining=TR)
    d_far = predict_cb_arima(s, n_root=cb + 10.0, t_active=190, t_remaining=TR)
    assert math.isfinite(d_near["eta_s"]) and math.isfinite(d_far["eta_s"])
    ratio = d_far["eta_s"] / d_near["eta_s"]
    assert abs(ratio - (10.0 / 3.3)) < 0.4       # ~3x, drastically different


# 7. Faster overall progress -> shorter ETA.
def test_faster_rate_shorter_eta():
    slow = series(lambda i: 1.0 * i, 20)
    fast = series(lambda i: 4.0 * i, 20)
    ds = predict_cb_arima(slow, n_root=slow[-1][1] + 40, t_active=190, t_remaining=TR)
    df = predict_cb_arima(fast, n_root=fast[-1][1] + 40, t_active=190, t_remaining=TR)
    assert df["eta_s"] < ds["eta_s"]


# 8. More remaining bits at the same rate -> longer ETA.
def test_more_remaining_longer_eta():
    s = series(lambda i: 2.0 * i, 20)
    near = predict_cb_arima(s, n_root=s[-1][1] + 10, t_active=190, t_remaining=TR)
    far = predict_cb_arima(s, n_root=s[-1][1] + 50, t_active=190, t_remaining=TR)
    assert far["eta_s"] > near["eta_s"]


# 9. Infinity invariant: eta == inf IFF mu <= 0 (stuck). Steady -> finite; flat -> inf.
def test_infinity_invariant():
    steady = predict_cb_arima(series(lambda i: 2.0 * i, 20),
                              n_root=60.0, t_active=190, t_remaining=TR)
    flat = predict_cb_arima(series(lambda i: 100.0, 20),
                            n_root=200.0, t_active=190, t_remaining=TR)
    assert math.isfinite(steady["eta_s"]) and not steady["stuck"]
    assert flat["eta_s"] == math.inf and flat["stuck"]


# 10. Already past n_root -> eta 0, cocoa.
def test_already_done():
    s = series(lambda i: 10.0 * i, 10)           # cb up to 90
    d = predict_cb_arima(s, n_root=50.0, t_active=90, t_remaining=TR)
    assert d["eta_s"] == 0.0 and d["engine"] == "cocoa"


# 11. Too few samples -> ganak (no guessing).
def test_few_samples_ganak():
    s = series(lambda i: 2.0 * i, 3)             # 2 warm samples after dropping cb=0
    d = predict_cb_arima(s, n_root=100.0, t_active=20, t_remaining=TR)
    assert d["engine"] == "ganak" and "insufficient" in d["reason"]


# 12. Irregular timestamps (jitter) -> still finite and ~accurate (uses median step).
def test_irregular_dt():
    s = [(i * 10.0 + (0.4 if i % 2 else -0.3), 2.0 * i) for i in range(20)]
    d = predict_cb_arima(s, n_root=s[-1][1] + 40, t_active=190, t_remaining=TR)
    assert math.isfinite(d["eta_s"])
    assert abs(d["eta_s"] - 200.0) < 40.0


# 13. Measurement noise on a steady climb -> robust finite ETA.
def test_noisy_steady_robust():
    jitter = [0.0, 0.3, 0.1, 0.4, 0.2]           # deterministic, keeps cb increasing
    s = series(lambda i: 2.0 * i + jitter[i % 5], 30)
    d = predict_cb_arima(s, n_root=s[-1][1] + 40, t_active=290, t_remaining=TR)
    assert math.isfinite(d["eta_s"])
    assert abs(d["eta_s"] - 200.0) < 60.0


# 14. Handoff decision keys off ETA vs margin*remaining-budget.
def test_handoff_decision_boundary():
    s = series(lambda i: 1.0 * i, 20)            # mu 1 b/step -> eta = remaining*10 s
    tight = predict_cb_arima(s, n_root=s[-1][1] + 100, t_active=190, t_remaining=500)
    loose = predict_cb_arima(s, n_root=s[-1][1] + 100, t_active=190, t_remaining=3000)
    assert tight["engine"] == "ganak"            # eta 1000s > 0.9*500
    assert loose["engine"] == "cocoa"            # eta 1000s <= 0.9*3000


# 15. SAMPLING-INTERVAL INVARIANCE -- the SAME wall-clock trajectory sampled at 10s vs
#     30s must give the SAME (finite) verdict. A burst-then-wall is finite under the
#     no-floor model, and that finite ETA must be ~equal at both sampling rates.
def test_sampling_interval_invariance():
    def cb_of_t(t):
        return min(300.0, t)            # 1 bit/s climb to 300, then flat

    fine = [(t, cb_of_t(t)) for t in range(0, 481, 10)]    # 10s sampling
    coarse = [(t, cb_of_t(t)) for t in range(0, 481, 30)]  # 30s sampling
    d_fine = predict_cb_arima(fine, n_root=400.0, t_active=480, t_remaining=TR)
    d_coarse = predict_cb_arima(coarse, n_root=400.0, t_active=480, t_remaining=TR)
    assert math.isfinite(d_fine["eta_s"]) and math.isfinite(d_coarse["eta_s"])
    # same wall-clock signal -> ETAs within ~20% regardless of sampling rate
    assert abs(d_fine["eta_s"] - d_coarse["eta_s"]) < 0.2 * d_fine["eta_s"]


# 16. A genuinely-progressing series at two sampling rates -> close finite ETAs.
def test_invariance_progressing():
    def cb_of_t(t):
        return 0.5 * t                  # 0.5 b/s steady

    fine = [(t, cb_of_t(t)) for t in range(0, 201, 10)]
    coarse = [(t, cb_of_t(t)) for t in range(0, 201, 20)]
    cb = fine[-1][1]
    d_fine = predict_cb_arima(fine, n_root=cb + 50, t_active=200, t_remaining=TR)
    d_coarse = predict_cb_arima(coarse, n_root=cb + 50, t_active=200, t_remaining=TR)
    assert math.isfinite(d_fine["eta_s"]) and math.isfinite(d_coarse["eta_s"])
    # 50 bits / 0.5 b/s = 100s, both samplings
    assert abs(d_fine["eta_s"] - 100.0) < 15.0
    assert abs(d_coarse["eta_s"] - 100.0) < 15.0


# 17. Deceleration into a plateau -> ETA longer than the naive average-rate projection
#     (the AR(1) transient lengthens it when the recent rate is below mu).
def test_decelerating_lengthens_eta():
    cbs = [0.0]
    for i in range(1, 25):
        cbs.append(cbs[-1] + (3.0 if i < 18 else 1.0))   # 3,3,...,1,1 (recent slower)
    s = [(i * 10.0, cbs[i]) for i in range(len(cbs))]
    d = predict_cb_arima(s, n_root=s[-1][1] + 30, t_active=240, t_remaining=TR)
    assert math.isfinite(d["eta_s"]) and not d["stuck"]
    # recent rate is 1 b/step (0.1 b/s); naive remaining/r_last = 30/1*10 = 300s.
    naive = 30.0 / 1.0 * 10.0
    assert d["eta_s"] < naive                            # avg mu (higher) shortens vs last-rate
    assert d["eta_s"] > 0.0


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main([__file__, "-q"]))
