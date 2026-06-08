"""Synthetic tests for the recency-weighted progress-rate forecaster (predict_cb_recency).

Model:  eta = remaining_bits / rate,  where
        rate = Sum_i w_i * r_i / Sum_i w_i,  r_i = per-interval cb rate (bits/sec),
        w_i  = (1 + age_i / TAU) ^ (-POWER)   -- power-law recency weight, age in SECONDS.

The eta is infinite IFF the recency-weighted rate <= 0 (no recent net progress). There is
no AR term, no drift-mean, no stuck-floor. Time-indexed in seconds, so the sampling
interval must not change the verdict (test_sampling_interval_invariance). The chosen
weights (POWER=2, TAU=120s) are provisional tuning knobs; these tests pin the *shape*
properties (recency reaction, heavy-tail memory, the remaining/rate discriminator), not
the exact constants.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from race.forecast import predict_cb_recency, RWR_TAU, RWR_POWER   # noqa: E402

STEP = 10.0
TR = 3000.0


def series(cb_of_i, n, step=STEP, t0=0.0):
    return [(t0 + i * step, float(cb_of_i(i))) for i in range(n)]


# 1. Steady progress -> finite, accurate ETA (equal rates -> weighted mean = that rate).
def test_steady_linear_accurate():
    s = series(lambda i: 2.0 * i, 20)                 # 2 bits/10s = 0.2 b/s
    d = predict_cb_recency(s, n_root=s[-1][1] + 40.0, t_active=190, t_remaining=TR)
    assert math.isfinite(d["eta_s"]) and not d["stuck"]
    assert abs(d["eta_s"] - 200.0) < 10.0             # 40 bits / 0.2 b/s
    assert d["engine"] == "cocoa"


# 2. Flat from the start -> rate 0 -> infinite ETA (stuck), ganak.
def test_flat_from_start_is_inf():
    s = series(lambda i: 100.0, 15)
    d = predict_cb_recency(s, n_root=200.0, t_active=140, t_remaining=TR)
    assert d["eta_s"] == math.inf and d["stuck"] and d["engine"] == "ganak"


# 3. Infinity invariant: eta == inf IFF the recency-weighted rate <= 0.
def test_infinity_invariant():
    steady = predict_cb_recency(series(lambda i: 2.0 * i, 20), n_root=60.0,
                                t_active=190, t_remaining=TR)
    flat = predict_cb_recency(series(lambda i: 100.0, 20), n_root=200.0,
                              t_active=190, t_remaining=TR)
    assert math.isfinite(steady["eta_s"]) and not steady["stuck"]
    assert flat["eta_s"] == math.inf and flat["stuck"]


# 4. More remaining bits at the same rate -> longer ETA.
def test_more_remaining_longer_eta():
    s = series(lambda i: 2.0 * i, 20)
    near = predict_cb_recency(s, n_root=s[-1][1] + 10, t_active=190, t_remaining=TR)
    far = predict_cb_recency(s, n_root=s[-1][1] + 50, t_active=190, t_remaining=TR)
    assert far["eta_s"] > near["eta_s"]


# 5. Faster overall rate -> shorter ETA.
def test_faster_rate_shorter_eta():
    slow = predict_cb_recency(series(lambda i: 1.0 * i, 20),
                              n_root=series(lambda i: 1.0 * i, 20)[-1][1] + 40,
                              t_active=190, t_remaining=TR)
    fast = predict_cb_recency(series(lambda i: 4.0 * i, 20),
                              n_root=series(lambda i: 4.0 * i, 20)[-1][1] + 40,
                              t_active=190, t_remaining=TR)
    assert fast["eta_s"] < slow["eta_s"]


# 6. RECENCY reacts: same history + same remaining, but a recent burst gives a shorter
#    ETA than a recent plateau (recent intervals weigh most).
def test_recency_reacts_to_recent():
    base = [(10.0 * i, 5.0 * i) for i in range(18)]            # climb to cb=85 by t=170
    burst = base + [(180., 92.), (190., 100.), (200., 108.)]   # recent burst
    plateau = base + [(180., 85.2), (190., 85.4), (200., 85.6)]  # recent crawl
    db = predict_cb_recency(burst, n_root=burst[-1][1] + 20, t_active=200, t_remaining=TR)
    dp = predict_cb_recency(plateau, n_root=plateau[-1][1] + 20, t_active=200, t_remaining=TR)
    assert db["eta_s"] < dp["eta_s"]                           # same remaining (=20), faster recent -> shorter


# 7. A burst-then-wall keeps a FINITE ETA (heavy tail remembers the burst) that GROWS
#    as the wall lengthens -- it never snaps to inf, but rises toward a bail.
def test_wall_eta_grows_smoothly_finite():
    climb = [(float(t), float(t)) for t in range(0, 101, 10)]   # cb 0..100, 1 b/s
    etas = []
    for w in range(1, 22):
        wall = [(100.0 + 10 * k, 100.0) for k in range(1, w + 1)]
        d = predict_cb_recency(climb + wall, n_root=130.0,
                               t_active=100 + 10 * w, t_remaining=TR)
        assert math.isfinite(d["eta_s"])                        # tail keeps it finite
        etas.append(d["eta_s"])
    assert all(b >= a - 1e-9 for a, b in zip(etas, etas[1:]))   # non-decreasing
    assert etas[-1] > 1.5 * etas[0]                             # meaningfully grows


# 8. HEAVY TAIL memory: a strong early burst then a long flat stays FINITE (the burst is
#    remembered), whereas a never-progressing trajectory is inf.
def test_heavy_tail_remembers_burst():
    burst = [(0., 0.), (10., 40.), (20., 41.)] + [(20.0 + 10 * k, 41.0) for k in range(1, 25)]
    flatonly = [(10.0 * k, 41.0) for k in range(28)]            # genuinely constant cb
    db = predict_cb_recency(burst, n_root=100.0, t_active=260, t_remaining=TR)
    df = predict_cb_recency(flatonly, n_root=100.0, t_active=270, t_remaining=TR)
    assert math.isfinite(db["eta_s"])                           # burst remembered -> finite
    assert df["eta_s"] == math.inf                              # no progress ever -> inf


# 9. THE DISCRIMINATOR (t1_045 vs 011 essence): same slow rate, but CLOSE-to-done keeps
#    while FAR-from-done bails. ETA = remaining/rate does this; the slope/level alone can't.
def test_close_keeps_far_bails():
    s = [(float(t), 50.0 + 0.05 * t) for t in range(0, 201, 10)]   # crawl 0.05 b/s
    cb = s[-1][1]
    close = predict_cb_recency(s, n_root=cb + 1.0, t_active=200, t_remaining=1000)   # ~1 bit left
    far = predict_cb_recency(s, n_root=cb + 60.0, t_active=200, t_remaining=1000)    # 60 bits left
    assert close["engine"] == "cocoa"      # ~20s << budget -> keep
    assert far["engine"] == "ganak"        # ~1200s > 0.9*1000 -> bail


# 10. Handoff decision keys off ETA vs margin * remaining-budget.
def test_handoff_decision_boundary():
    s = series(lambda i: 1.0 * i, 20)                 # 0.1 b/s
    tight = predict_cb_recency(s, n_root=s[-1][1] + 100, t_active=190, t_remaining=500)
    loose = predict_cb_recency(s, n_root=s[-1][1] + 100, t_active=190, t_remaining=3000)
    assert tight["engine"] == "ganak"                 # eta 1000s > 0.9*500
    assert loose["engine"] == "cocoa"                 # eta 1000s <= 0.9*3000


# 11. SAMPLING-INTERVAL INVARIANCE: same wall-clock trajectory at 10s vs 30s -> ~same ETA
#     (weights are time-based, age in seconds).
def test_sampling_interval_invariance():
    cb = lambda t: 0.5 * t
    fine = [(t, cb(t)) for t in range(0, 201, 10)]
    coarse = [(t, cb(t)) for t in range(0, 201, 30)]
    df = predict_cb_recency(fine, n_root=fine[-1][1] + 50, t_active=200, t_remaining=TR)
    dc = predict_cb_recency(coarse, n_root=coarse[-1][1] + 50, t_active=200, t_remaining=TR)
    assert math.isfinite(df["eta_s"]) and math.isfinite(dc["eta_s"])
    assert abs(df["eta_s"] - dc["eta_s"]) < 0.15 * df["eta_s"]
    assert abs(df["eta_s"] - 100.0) < 15.0            # 50 bits / 0.5 b/s


# 12. Already past n_root -> eta 0, cocoa.
def test_already_done():
    s = series(lambda i: 10.0 * i, 10)
    d = predict_cb_recency(s, n_root=50.0, t_active=90, t_remaining=TR)
    assert d["eta_s"] == 0.0 and d["engine"] == "cocoa"


# 13. Too few samples -> ganak (no guessing).
def test_few_samples_ganak():
    s = series(lambda i: 2.0 * i, 3)                  # 2 warm samples after dropping cb=0
    d = predict_cb_recency(s, n_root=100.0, t_active=20, t_remaining=TR)
    assert d["engine"] == "ganak" and "insufficient" in d["reason"]


# 14. Irregular timestamps (jitter) -> still finite and ~accurate (rate is per-second).
def test_irregular_dt():
    s = [(i * 10.0 + (0.4 if i % 2 else -0.3), 2.0 * i) for i in range(20)]
    d = predict_cb_recency(s, n_root=s[-1][1] + 40, t_active=190, t_remaining=TR)
    assert math.isfinite(d["eta_s"])
    assert abs(d["eta_s"] - 200.0) < 40.0


# 15. The provisional knobs are exposed and echoed (so the trace records them).
def test_knobs_exposed():
    s = series(lambda i: 2.0 * i, 20)
    d = predict_cb_recency(s, n_root=s[-1][1] + 40, t_active=190, t_remaining=TR)
    assert d["tau"] == RWR_TAU and d["power"] == RWR_POWER


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main([__file__, "-q"]))
