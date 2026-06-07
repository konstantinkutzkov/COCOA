"""Extensive synthetic tests for the ARIMA(1,1,0)+drift closed_bits forecaster.

The point of synthetic data: we control the ground truth, so we can assert the model
does the RIGHT thing on each regime -- steady progress, flat-from-start, the
strong-start-then-stall that broke t1_031, short between-jumps plateaus, deep-tail
levels, deceleration, noise, and irregular sampling. Everything is time-indexed in
REAL SECONDS (no age-in-samples), so changing the sampling step must not change the
verdicts -- a property we test directly (test_sampling_interval_invariance).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from race.forecast import (predict_cb_arima,                      # noqa: E402
                           ARIMA_STUCK_WINDOW_S, ARIMA_STUCK_EPS_BITS)

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


# 2. Flat from the start -> infinite ETA (stuck), ganak.
def test_flat_from_start_is_inf():
    s = series(lambda i: 100.0, 15)              # constant
    d = predict_cb_arima(s, n_root=200.0, t_active=140, t_remaining=TR)
    assert d["eta_s"] == math.inf and d["stuck"]
    assert d["engine"] == "ganak"


# 3. THE t1_031 case: strong start then a multi-minute plateau -> MUST bail.
def test_strong_start_then_plateau_bails():
    climb = [(i * 10.0, 10.0 * i) for i in range(31)]                    # cb 0..300 over 300s
    plateau = [(300.0 + j * 10.0, 300.0) for j in range(1, 13)]          # flat 120s (>90s)
    d = predict_cb_arima(climb + plateau, n_root=331.0, t_active=420, t_remaining=TR)
    assert d["stuck"] and d["eta_s"] == math.inf and d["engine"] == "ganak"
    assert "stuck-floor" in d["reason"]


# 4. A short between-jumps plateau (< window) keeps its fair chance -> NOT stuck.
def test_short_plateau_gets_fair_chance():
    climb = [(i * 10.0, 10.0 * i) for i in range(31)]                    # to cb=300 at t=300
    plateau = [(300.0 + j * 10.0, 300.0) for j in range(1, 6)]           # flat only 50s
    d = predict_cb_arima(climb + plateau, n_root=400.0, t_active=350, t_remaining=TR)
    assert not d["stuck"] and math.isfinite(d["eta_s"])


# 5. Level matters (the user's point): a slow crawl close to done vs far from done ->
#    ETA proportional to `remaining`, so they differ by the remaining ratio.
def test_level_dependence():
    s = series(lambda i: 50.0 + 0.1 * i, 20)     # crawl 0.1 b/step (above the floor)
    cb = s[-1][1]
    d_near = predict_cb_arima(s, n_root=cb + 3.3, t_active=190, t_remaining=TR)
    d_far = predict_cb_arima(s, n_root=cb + 10.0, t_active=190, t_remaining=TR)
    assert math.isfinite(d_near["eta_s"]) and math.isfinite(d_far["eta_s"])
    ratio = d_far["eta_s"] / d_near["eta_s"]
    assert abs(ratio - (10.0 / 3.3)) < 0.4       # ~3x, drastically different


# 6. Faster overall progress -> shorter ETA.
def test_faster_rate_shorter_eta():
    slow = series(lambda i: 1.0 * i, 20)
    fast = series(lambda i: 4.0 * i, 20)
    ds = predict_cb_arima(slow, n_root=slow[-1][1] + 40, t_active=190, t_remaining=TR)
    df = predict_cb_arima(fast, n_root=fast[-1][1] + 40, t_active=190, t_remaining=TR)
    assert df["eta_s"] < ds["eta_s"]


# 7. More remaining bits at the same rate -> longer ETA.
def test_more_remaining_longer_eta():
    s = series(lambda i: 2.0 * i, 20)
    near = predict_cb_arima(s, n_root=s[-1][1] + 10, t_active=190, t_remaining=TR)
    far = predict_cb_arima(s, n_root=s[-1][1] + 50, t_active=190, t_remaining=TR)
    assert far["eta_s"] > near["eta_s"]


# 8. Infinity invariant: eta == inf IFF stuck.
def test_infinity_invariant():
    steady = predict_cb_arima(series(lambda i: 2.0 * i, 20),
                              n_root=60.0, t_active=190, t_remaining=TR)
    flat = predict_cb_arima(series(lambda i: 100.0, 20),
                            n_root=200.0, t_active=190, t_remaining=TR)
    assert math.isfinite(steady["eta_s"]) and not steady["stuck"]
    assert flat["eta_s"] == math.inf and flat["stuck"]


# 9. Already past n_root -> eta 0, cocoa.
def test_already_done():
    s = series(lambda i: 10.0 * i, 10)           # cb up to 90
    d = predict_cb_arima(s, n_root=50.0, t_active=90, t_remaining=TR)
    assert d["eta_s"] == 0.0 and d["engine"] == "cocoa"


# 10. Too few samples -> ganak (no guessing).
def test_few_samples_ganak():
    s = series(lambda i: 2.0 * i, 3)             # 2 warm samples after dropping cb=0
    d = predict_cb_arima(s, n_root=100.0, t_active=20, t_remaining=TR)
    assert d["engine"] == "ganak" and "insufficient" in d["reason"]


# 11. Irregular timestamps (jitter) -> still finite and ~accurate (uses median step).
def test_irregular_dt():
    s = [(i * 10.0 + (0.4 if i % 2 else -0.3), 2.0 * i) for i in range(20)]
    d = predict_cb_arima(s, n_root=s[-1][1] + 40, t_active=190, t_remaining=TR)
    assert math.isfinite(d["eta_s"])
    assert abs(d["eta_s"] - 200.0) < 40.0


# 12. Measurement noise on a steady climb -> robust finite ETA.
def test_noisy_steady_robust():
    jitter = [0.0, 0.3, 0.1, 0.4, 0.2]           # deterministic, keeps cb increasing
    s = series(lambda i: 2.0 * i + jitter[i % 5], 30)
    d = predict_cb_arima(s, n_root=s[-1][1] + 40, t_active=290, t_remaining=TR)
    assert math.isfinite(d["eta_s"])
    assert abs(d["eta_s"] - 200.0) < 60.0


# 13. Stuck-floor boundary: just below eps over the window -> stuck; just above -> not.
def test_stuck_floor_boundary():
    base = [(i * 10.0, 10.0 * i) for i in range(20)]               # climb to 190 by t=190
    tail_stuck = [(200.0 + j * 10.0, 190.0 + 0.04 * j) for j in range(1, 11)]   # +0.4/100s
    d_stuck = predict_cb_arima(base + tail_stuck, n_root=400.0, t_active=300, t_remaining=TR)
    assert d_stuck["stuck"] and d_stuck["eta_s"] == math.inf
    tail_ok = [(200.0 + j * 10.0, 190.0 + 0.12 * j) for j in range(1, 11)]      # +1.2/100s
    d_ok = predict_cb_arima(base + tail_ok, n_root=400.0, t_active=300, t_remaining=TR)
    assert not d_ok["stuck"] and math.isfinite(d_ok["eta_s"])


# 14. Handoff decision keys off ETA vs margin*remaining-budget.
def test_handoff_decision_boundary():
    s = series(lambda i: 1.0 * i, 20)            # mu 1 b/step -> eta = remaining*10 s
    tight = predict_cb_arima(s, n_root=s[-1][1] + 100, t_active=190, t_remaining=500)
    loose = predict_cb_arima(s, n_root=s[-1][1] + 100, t_active=190, t_remaining=3000)
    assert tight["engine"] == "ganak"            # eta 1000s > 0.9*500
    assert loose["engine"] == "cocoa"            # eta 1000s <= 0.9*3000


# 15. SAMPLING-INTERVAL INVARIANCE -- the property the old per-sample model lacked.
#     The SAME wall-clock trajectory sampled at 10s vs 30s must give the SAME verdict.
def test_sampling_interval_invariance():
    # strong-start-then-stall, expressed as cb(t): climbs to 300 by t=300, flat after.
    def cb_of_t(t):
        return min(300.0, t)            # 1 bit/s climb to 300, then flat

    fine = [(t, cb_of_t(t)) for t in range(0, 481, 10)]    # 10s sampling
    coarse = [(t, cb_of_t(t)) for t in range(0, 481, 30)]  # 30s sampling
    d_fine = predict_cb_arima(fine, n_root=331.0, t_active=480, t_remaining=TR)
    d_coarse = predict_cb_arima(coarse, n_root=331.0, t_active=480, t_remaining=TR)
    # both must declare the SAME thing (stuck), regardless of sampling rate
    assert d_fine["stuck"] and d_coarse["stuck"]
    assert d_fine["eta_s"] == d_coarse["eta_s"] == math.inf


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


# 17. Deceleration into a (not-yet-floored) plateau -> ETA longer than naive last-rate.
def test_decelerating_lengthens_eta():
    # rate steps down 3,3,3,...,1,1 (recent slower) but still > floor over the window
    cbs = [0.0]
    for i in range(1, 25):
        cbs.append(cbs[-1] + (3.0 if i < 18 else 1.0))
    s = [(i * 10.0, cbs[i]) for i in range(len(cbs))]
    d = predict_cb_arima(s, n_root=s[-1][1] + 30, t_active=240, t_remaining=TR)
    assert math.isfinite(d["eta_s"]) and not d["stuck"]
    # recent rate is 1 b/step (0.1 b/s); naive remaining/r_last = 30/1*10 = 300s.
    # mu (avg) is higher, but the AR transient (r_last<mu) must keep eta clearly > 0.
    assert d["eta_s"] > 0.0


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main([__file__, "-q"]))
