"""Self-contained regression tests for the escape-history handoff forecaster
(predict_cb_tree). Synthetic trajectories only -- no trace-file dependencies.

The full keep/bail validation against the 7 real leader traces lives in
portfolio/_esc_harness.py (run_and_print); these pin the core CONTRACT so it
cannot silently regress:
  * a lone jump (from 0 OR a positive baseline) is NOT a plateau-escape -> bail;
  * a config that has escaped plateaus is kept while within k x its escape envelope,
    and bailed once stalled beyond it;
  * currently-progressing / too-few-samples -> keep; n_root None/inf -> no crash.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from race.forecast import predict_cb_tree   # noqa: E402


def verdict(traj, n_root, t_remaining=3000.0):
    return predict_cb_tree(traj, n_root, traj[-1][0], t_remaining)["verdict"]


# 1. Cliff jump from 0, then a dead wall (the mc2026_017 essence). The 0->X jump has no
#    preceding plateau -> not an escape -> escapes empty -> bail.
def test_cliff_from_zero_then_wall_bails():
    traj = [(10., 0.), (20., 5000.)] + [(float(t), 5000.) for t in range(30, 401, 10)]
    assert verdict(traj, 5002.0) == "bail"


# 2. THE HOSTAGE: cliff jump from a POSITIVE baseline then a wall. The jump's pre-plateau
#    (10s) is below MIN_PLAT, so it is NOT an escape -> bail. (The recency forecaster rode
#    this to timeout; the tree must hand off.)
def test_hostage_cliff_from_positive_baseline_bails():
    traj = [(10., 50.), (20., 50.), (30., 5000.)] + [(float(t), 5000.) for t in range(40, 401, 10)]
    assert verdict(traj, 5002.0) == "bail"


# 3. Currently progressing AND pct-feasible: a healthy ~0.5 b/s climb with only ~6 bits left
#    -> keep, regardless of escape history. (n_root set so the pct log-budget ~11 bits exceeds
#    the 6 remaining; the SAME rate far from done is doomed -- see #11.)
def test_progressing_feasible_keeps():
    traj = [(float(10 * i), 5.0 * i) for i in range(1, 31)]   # steady 0.5 b/s, cb_now=150
    assert verdict(traj, 156.0) == "keep"                      # remaining 6 < pct log-budget ~11


# 4. Too few samples -> keep (insufficient data to judge).
def test_few_samples_keep():
    assert verdict([(10., 1.), (20., 2.), (30., 3.)], 100.0) == "keep"


# 5. A demonstrated recoverer (two ~60s-plateau escapes) whose last escape is still inside the
#    180s recency window, so recent_rate>eps -> judged by the (pct) eta-gate; with only ~5 bits
#    left it is pct-feasible -> keep. (The escape-envelope BAIL is pinned by #6.)
def _recoverer():
    return ([(10., 100.)] + [(float(t), 100.) for t in range(20, 71, 10)]   # 60s plateau
            + [(80., 110.)]                                                  # escape 1
            + [(float(t), 110.) for t in range(90, 141, 10)]                 # 60s plateau
            + [(150., 120.)]                                                 # escape 2
            + [(float(t), 120.) for t in range(160, 311, 10)])               # current plateau ~160s


def test_recoverer_progressing_feasible_keeps():
    assert verdict(_recoverer(), 125.0) == "keep"   # remaining ~5 bits < pct log-budget ~7.8


# 6. Same recoverer, but stalled ~200s -> beyond k x escape envelope (180) -> bail.
def test_recoverer_beyond_envelope_bails():
    traj = _recoverer() + [(float(t), 120.) for t in range(320, 351, 10)]
    assert verdict(traj, 200.0) == "bail"


# 7. n_root unknown (None) or non-finite (inf) must not crash; a stalled no-escape config bails.
def test_unknown_or_infinite_n_root_no_crash():
    traj = [(10., 0.), (20., 5000.)] + [(float(t), 5000.) for t in range(30, 401, 10)]
    assert predict_cb_tree(traj, None, traj[-1][0], 3000.0)["verdict"] == "bail"
    assert predict_cb_tree(traj, math.inf, traj[-1][0], 3000.0)["verdict"] == "bail"


# 8. Already complete (remaining <= 0) -> keep (nothing to hand off).
def test_complete_keeps():
    traj = [(float(10 * i), 5.0 * i) for i in range(1, 21)]
    assert verdict(traj, 10.0) == "keep"   # n_root below current cb


# 9. mc2026_027 REGRESSION: PROGRESSING (recent_rate>eps) but far from done AND too slow --
#    eta blows the budget -> bail. The pre-fix tree only asked "moving?" not "fast enough?",
#    so it rode this leader for ~16 min of active time (a 70-min wall runaway).
def test_progressing_but_doomed_far_and_slow_bails():
    traj = [(float(t), 0.005 * t) for t in range(10, 601, 10)]   # steady ~0.005 b/s creep, cb_now=3.0
    # remaining = 30 - 3 = 27 bits (>> floor); eta = 27/0.005 = 5400s > 2 x 2000s budget -> bail
    assert verdict(traj, 30.0, t_remaining=2000.0) == "bail"


# 10. ...but a near-finisher (remaining <= KEEP_REM_FLOOR) is ALWAYS ridden out, even when slow
#     and even when its eta nominally exceeds the (small) remaining budget -- the 025 case (91%).
def test_progressing_near_finisher_kept_despite_slow():
    traj = [(float(t), 0.005 * t) for t in range(10, 601, 10)]   # same creep, cb_now=3.0
    # remaining = 4 - 3 = 1 bit (< floor 2); eta = 1/0.005 = 200s > 2 x 50s, but the floor keeps it
    assert verdict(traj, 4.0, t_remaining=50.0) == "keep"


# 11. mc2026_047 (REAL trajectory): a leader creeping at ~0.012 b/s with ~8 bits left LOOKS
#     feasible to the old LINEAR gate (eta = 8/0.012 ~ 660s << budget -> keep) but is pct-doomed
#     -- 8 more bits means ~2^8x the linear count still to enumerate, at a rate that had closed
#     only ~2.4% of it. The log-space pct-gate bails. (In production the old linear gate rode
#     this ~25 min until the frozen-path handoff; the pct-gate would have bailed ~18-20 min in.)
def test_mc2026_047_real_trajectory_pct_doomed_bails():
    cbs = [79.84, 80.43, 81.33, 82.02]                       # observed monitoring cb @ 60s spacing
    traj = [(1080.0 + 60.0 * i, cb) for i, cb in enumerate(cbs)]
    # n_root pinned at 90; remaining 7.98 bits; recent_rate ~0.0121 b/s; t_remaining 2520s.
    # OLD: eta = 7.98/0.0121 ~ 660s < 2x2520 -> KEEP (the bug). NEW: log2(ln2*0.0121*2*2520)
    # ~ 5.4 < 7.98 -> BAIL.
    assert predict_cb_tree(traj, 90.0, traj[-1][0], 2520.0)["verdict"] == "bail"


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main([__file__, "-q"]))
