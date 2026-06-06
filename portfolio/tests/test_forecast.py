"""Tests for race.forecast — the round-2 COCOA-vs-Ganak handoff forecaster.

Heavy emphasis on the BASIC cases that the previous power-law model got wrong:
the central regression is that a slowly-but-steadily progressing trajectory must
yield a FINITE ETA (the old model reported inf and wrongly handed off). The single
legitimate infinity is a genuinely flat/regressing recent rate.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # portfolio/
from race.forecast import predict, progress_bonus  # noqa: E402

T_REM = 2880.0


def _linear(rate, p0=0.0, ts=range(10, 181, 10)):
    """A trajectory rising at a constant `rate` %/s (so progress_bonus ~ 0)."""
    return [(t, p0 + rate * t) for t in ts]


# Real leader trajectories captured from full-budget runs (2026-06-05).
T1_013 = [(10, 50.6711), (20, 50.6711), (30, 53.7509), (40, 53.8005), (50, 53.8809),
          (60, 53.883), (70, 53.8836), (80, 53.8836), (90, 53.8836), (100, 53.8836),
          (110, 53.8836), (120, 55.4691), (130, 55.6506), (140, 55.8459), (150, 55.9496),
          (160, 55.9496), (170, 55.9496), (180, 55.9496)]
T1_101 = [(10, 0.0), (20, 0.00672), (30, 0.05245), (40, 0.05301), (50, 0.05319),
          (60, 0.05338), (70, 0.05398), (80, 0.05417), (90, 0.05474), (100, 0.05492),
          (110, 0.05493), (120, 0.05493), (130, 0.05789), (141, 0.05792), (151, 0.05794),
          (161, 0.05798), (171, 0.05949), (180, 0.06065)]


# ---------------------------------------------------------------------------
# THE central regression: slow-but-positive progress -> finite ETA, never inf.
# ---------------------------------------------------------------------------
def test_slow_steady_progress_is_finite_not_inf():
    d = predict(_linear(0.03), t_active=180, t_remaining=T_REM)   # ~5.4% @180s
    assert math.isfinite(d["eta_s"]), "slow steady progress must give a FINITE eta"
    assert d["engine"] == "ganak"     # finite but far beyond budget -> hand off


def test_no_infinity_for_any_positive_rate():
    """Property: every positive recent rate yields a finite ETA."""
    for rate in (1e-5, 1e-4, 1e-3, 0.01, 0.1, 1.0):
        d = predict(_linear(rate), t_active=180, t_remaining=T_REM)
        assert math.isfinite(d["eta_s"]), f"rate={rate} produced non-finite eta"


def test_only_flat_trajectory_is_infinite():
    d = predict([(t, 0.3) for t in range(10, 181, 10)], 180, T_REM)
    assert d["eta_s"] == math.inf
    assert d["engine"] == "ganak"


def test_regressing_rate_is_infinite_and_ganak():
    # pct_lin going DOWN (shouldn't happen, but must not crash / must hand off)
    traj = [(t, max(5.0 - 0.01 * t, 0.0)) for t in range(10, 181, 10)]
    d = predict(traj, 180, T_REM)
    assert d["eta_s"] == math.inf and d["engine"] == "ganak"


# ---------------------------------------------------------------------------
# Basic decisions.
# ---------------------------------------------------------------------------
def test_fast_finisher_is_cocoa():
    d = predict(_linear(0.5), 180, T_REM)        # ~90% @180s, rate 0.5%/s
    assert d["engine"] == "cocoa"
    assert math.isfinite(d["eta_s"]) and d["eta_s"] < T_REM


def test_already_home_is_cocoa():
    d = predict([(t, 100.0) for t in range(10, 181, 10)], 180, T_REM)
    assert d["engine"] == "cocoa"
    assert d["eta_s"] == 0.0


def test_insufficient_samples_is_ganak():
    assert predict([(180, 5.0)], 180, T_REM)["engine"] == "ganak"
    assert predict([], 180, T_REM)["engine"] == "ganak"


def test_monotonic_in_rate():
    """Faster steady progress => not-larger ETA."""
    etas = [predict(_linear(r), 180, T_REM)["eta_s"] for r in (0.01, 0.05, 0.2, 0.5)]
    assert all(math.isfinite(e) for e in etas)
    assert etas == sorted(etas, reverse=True)   # strictly non-increasing


def test_rate_weights_recent_samples_more():
    """Recency-weighted rate over ALL samples: with the SAME total progress, a
    trajectory that plateaus AT THE END projects a slower rate (larger ETA) than one
    that plateaus at the START. Every sample counts, but recent ones dominate."""
    ts = list(range(10, 181, 10))
    end_flat = [(t, min(50.0, 50.0 * t / 90.0)) for t in ts]                  # climb, then flat
    start_flat = [(t, 0.0 if t < 90 else 50.0 * (t - 90) / 90.0) for t in ts]  # flat, then climb
    d_end = predict(end_flat, 180, T_REM)
    d_start = predict(start_flat, 180, T_REM)
    assert d_end["current_pct_lin"] == d_start["current_pct_lin"] == 50.0      # same total
    assert d_end["rate_pct_per_s"] < d_start["rate_pct_per_s"]                 # recent plateau slower
    assert d_end["eta_s"] > d_start["eta_s"]
    assert math.isfinite(d_end["eta_s"])                                       # but still finite


# ---------------------------------------------------------------------------
# Bonus properties.
# ---------------------------------------------------------------------------
def test_bonus_nonnegative():
    for traj in (T1_013, T1_101, _linear(0.03), _linear(0.5)):
        assert progress_bonus(traj) >= 0.0


def test_bonus_scale_invariant():
    """Multiplying every progress value by a constant leaves the bonus unchanged."""
    base = progress_bonus(T1_101)
    for c in (2, 5, 10, 100):
        scaled = [(t, c * p) for t, p in T1_101]
        assert math.isclose(progress_bonus(scaled), base, rel_tol=1e-9)


def test_bonus_zero_for_perfectly_steady():
    assert progress_bonus(_linear(0.1)) == 0.0   # all deltas equal -> no excess


def test_bonus_rewards_recent_peaks_more():
    """Same-size spike counts for more when it is recent than when it is early."""
    flat = [(t, 0.01 * i) for i, t in enumerate(range(10, 181, 10), start=1)]
    early = [list(p) for p in flat]
    late = [list(p) for p in flat]
    # inject an equal-sized extra jump early (interval 3) vs late (interval 16),
    # then carry it forward so later cumulative values stay monotone
    for j in range(2, len(early)):
        early[j][1] += 1.0
    for j in range(15, len(late)):
        late[j][1] += 1.0
    b_early = progress_bonus([tuple(p) for p in early])
    b_late = progress_bonus([tuple(p) for p in late])
    assert b_late > b_early


def test_bonus_only_shrinks_eta():
    """eta_with_bonus <= base_eta (B >= 0 => divide by >=1)."""
    d = predict(T1_013, 180, 2879.0)
    assert d["eta_s"] <= d["base_eta_s"] + 1e-9


# ---------------------------------------------------------------------------
# Real-trajectory regressions (the cases that motivated the rewrite).
# ---------------------------------------------------------------------------
def test_t1_013_real_trajectory_finite_and_kept_on_cocoa():
    """t1_013 leader (nosep-cascade) at 56% with a recent bump: must be FINITE,
    and with default knobs the bonus keeps it on COCOA (the intended fix; note the
    ground-truth outcome is unknown -- this pins model behavior under the knobs)."""
    d = predict(T1_013, t_active=180, t_remaining=2879.0)
    assert math.isfinite(d["eta_s"])
    assert d["engine"] == "cocoa"


def test_t1_101_real_trajectory_finite_and_ganak():
    """t1_101 leader is genuinely stuck at 0.06%: must hand off to Ganak, but the
    ETA must be FINITE (this is exactly where the old model returned inf)."""
    d = predict(T1_101, t_active=180, t_remaining=2880.0)
    assert d["engine"] == "ganak"
    assert math.isfinite(d["eta_s"]), "t1_101 must be finite, not inf"


def test_t1_101_scaled_x10_still_finite_and_ganak():
    scaled = [(t, 10 * p) for t, p in T1_101]   # 0.6% @180s
    d = predict(scaled, t_active=180, t_remaining=2880.0)
    assert d["engine"] == "ganak"
    assert math.isfinite(d["eta_s"])
    # bonus identical to unscaled (scale-invariance), only base ETA shrank
    assert math.isclose(d["bonus_B"], predict(T1_101, 180, 2880.0)["bonus_B"], rel_tol=1e-9)


# ---------------------------------------------------------------------------
# Caller contract (scheduler.py uses fc["engine"] and fc["reason"]).
# ---------------------------------------------------------------------------
def test_return_contract():
    for traj in (T1_013, T1_101, [], [(180, 5.0)], _linear(0.5)):
        d = predict(traj, 180, T_REM)
        assert d["engine"] in ("cocoa", "ganak")
        assert isinstance(d["reason"], str) and d["reason"]


if __name__ == "__main__":
    # allow running without pytest
    import traceback
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    passed = 0
    for fn in fns:
        try:
            fn()
            passed += 1
            print(f"  PASS  {fn.__name__}")
        except Exception:
            print(f"  FAIL  {fn.__name__}")
            traceback.print_exc()
    print(f"\n===== {passed}/{len(fns)} tests PASS =====")
    sys.exit(0 if passed == len(fns) else 1)
