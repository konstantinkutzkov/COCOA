"""Unit tests for predict_cb — the closed_bits time-series forecaster.

Pins the invariants the old pct_lin model violated (the "infinity mess"):
  * eta is inf IFF the closed_bits rate <= 0 (genuinely stuck) -- never otherwise;
  * deep-tail instances (pct_lin ~ 0) stay RANKABLE via closed_bits (the whole point);
  * monotonicity (faster / closer / smaller-target => lower ETA);
  * the jump bonus on closed_bits deltas (steady=0, jump>0, scale-invariant, recency).
Plus real-data regression locks (t1_045 finisher, t1_089 plateau).

Run: pytest tests/test_predict_cb.py    (or: python tests/test_predict_cb.py)
"""
import math, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))          # portfolio/ -> race.forecast
from race.forecast import predict_cb, progress_bonus

TR = 2880.0          # typical round-3 remaining budget
NROOT = 100.0
TS = list(range(10, 181, 10))                       # 18 samples, 10s apart

def cb(ts, rate, cb0=10.0):
    return [(t, cb0 + rate * t) for t in ts]

# ---------------- INFINITY INVARIANT: eta == inf IFF cb rate <= 0 ----------------
def test_flat_cb_is_inf_and_ganak():
    d = predict_cb([(t, 50.0) for t in TS], NROOT, 180, TR)
    assert d["eta_s"] == math.inf and d["engine"] == "ganak"

def test_any_positive_rate_is_finite():
    d = predict_cb(cb(TS, 0.001), NROOT, 180, TR)   # crawling, but moving
    assert math.isfinite(d["eta_s"])

def test_inf_iff_nonpositive_rate():
    for rate in (-0.02, 0.0, 1e-6, 0.01, 0.2):
        d = predict_cb(cb(TS, rate), NROOT, 180, TR)
        if rate <= 0:
            assert d["eta_s"] == math.inf, f"rate={rate} should be inf"
        else:
            assert math.isfinite(d["eta_s"]), f"rate={rate} should be finite"

# ---------------- DEGENERACY FIX: deep-tail stays rankable ----------------
def test_deeptail_rankable_and_ordered():
    # mimic t1_001: pct_lin would be ~0, but closed_bits moves. Both far from n_root;
    # the faster cb-rate must get the lower (finite) ETA -> a usable ranking.
    slow = predict_cb(cb(TS, 0.01, cb0=1050.0), 1190.0, 180, TR)
    fast = predict_cb(cb(TS, 0.05, cb0=1050.0), 1190.0, 180, TR)
    assert math.isfinite(slow["eta_s"]) and math.isfinite(fast["eta_s"])
    assert fast["eta_s"] < slow["eta_s"]

# ---------------- BASIC DECISIONS ----------------
def test_fast_finisher_cocoa():
    d = predict_cb(cb(TS, 0.3), NROOT, 180, TR)      # cb 10->64, remaining 36, eta small
    assert d["engine"] == "cocoa" and 0.0 < d["eta_s"] < math.inf

def test_slow_crawler_ganak():
    d = predict_cb(cb(TS, 0.001), NROOT, 180, TR)    # remaining ~90 at 0.001 b/s -> huge eta
    assert d["engine"] == "ganak"

def test_already_done_eta_zero():
    d = predict_cb([(t, 100.0 + t) for t in TS], NROOT, 180, TR)  # cb >= n_root
    assert d["engine"] == "cocoa" and d["eta_s"] == 0.0

def test_too_few_samples_ganak():
    d = predict_cb([(10, 50.0), (20, 51.0)], NROOT, 20, TR)       # < MIN_SAMPLES
    assert d["engine"] == "ganak" and d["eta_s"] is None

# ---------------- MONOTONICITY ----------------
def test_higher_rate_lower_eta():
    assert predict_cb(cb(TS, 0.04), NROOT, 180, TR)["eta_s"] < \
           predict_cb(cb(TS, 0.02), NROOT, 180, TR)["eta_s"]

def test_closer_to_nroot_lower_eta():
    assert predict_cb(cb(TS, 0.02, cb0=80.0), NROOT, 180, TR)["eta_s"] < \
           predict_cb(cb(TS, 0.02, cb0=10.0), NROOT, 180, TR)["eta_s"]

def test_larger_nroot_higher_eta():
    assert predict_cb(cb(TS, 0.02), 200.0, 180, TR)["eta_s"] > \
           predict_cb(cb(TS, 0.02), 50.0, 180, TR)["eta_s"]

# ---------------- JUMP BONUS on closed_bits deltas (include_open=False) ----------------
def test_steady_cb_zero_bonus():
    assert progress_bonus(cb(TS, 0.03), include_open=False) < 1e-9   # steady -> B~0 (float drift)

def test_recent_jump_positive_bonus():
    pts = [(10, 10), (20, 10.1), (30, 10.2), (40, 10.3), (50, 15.0)]
    assert progress_bonus(pts, include_open=False) > 0.0

def test_jump_shrinks_eta():
    steady = cb(TS, 0.03)
    jumpy = steady[:-1] + [(TS[-1], steady[-1][1] + 5.0)]
    assert predict_cb(jumpy, NROOT, 180, TR)["eta_s"] < predict_cb(steady, NROOT, 180, TR)["eta_s"]

def test_bonus_scale_invariant():
    base = [(10, 10), (20, 10.5), (30, 10.6), (40, 12.0), (50, 12.2)]
    scaled = [(t, 10 + (c - 10) * 3.0) for (t, c) in base]    # deltas x3
    assert abs(progress_bonus(base, include_open=False) -
               progress_bonus(scaled, include_open=False)) < 1e-9

def test_bonus_recency_weighting():
    old_jump = [(10, 10), (20, 15), (30, 15.1), (40, 15.2), (50, 15.3)]
    new_jump = [(10, 10), (20, 10.1), (30, 10.2), (40, 10.3), (50, 15.3)]
    assert progress_bonus(new_jump, include_open=False) > progress_bonus(old_jump, include_open=False)

# ---------------- WARM-UP: leading cb<=0 points dropped ----------------
def test_warmup_zeros_dropped():
    pts = [(10, 0.0), (20, 0.0), (30, 20.6), (40, 20.8), (50, 21.0), (60, 21.2)]
    d = predict_cb(pts, NROOT, 60, TR)
    assert d["n_samples"] == 4 and math.isfinite(d["eta_s"])

# ---------------- REAL-DATA REGRESSION (lock the backtest findings) ----------------
def test_real_t1089_plateau_is_ganak():
    pts = [(180, 475.845), (240, 475.906), (600, 475.997),
           (900, 476.07), (1200, 476.14), (1500, 476.18)]
    assert predict_cb(pts, 480.0, 1500, 3600 - 1500)["engine"] == "ganak"

def test_real_t1045_finisher_is_cocoa():
    pts = [(60, 81.68), (240, 85.43), (600, 88.46), (1200, 89.70), (1800, 89.88)]
    d = predict_cb(pts, 90.0, 1800, 3600 - 1800)
    assert d["engine"] == "cocoa" and math.isfinite(d["eta_s"])


if __name__ == "__main__":
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    n = 0
    for f in fns:
        try:
            f(); n += 1; print(f"  PASS  {f.__name__}")
        except AssertionError as e:
            print(f"  FAIL  {f.__name__}: {e}")
    print(f"\n===== predict_cb: {n}/{len(fns)} PASS =====")
    sys.exit(0 if n == len(fns) else 1)
