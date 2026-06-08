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


# 3. Currently progressing (steady climb) -> keep, regardless of escape history.
def test_progressing_keeps():
    traj = [(float(10 * i), 5.0 * i) for i in range(1, 31)]
    assert verdict(traj, 500.0) == "keep"


# 4. Too few samples -> keep (insufficient data to judge).
def test_few_samples_keep():
    assert verdict([(10., 1.), (20., 2.), (30., 3.)], 100.0) == "keep"


# 5. A demonstrated recoverer (two ~60s-plateau escapes) currently in a ~160s plateau,
#    which is past GRACE(120) but within k(3) x max_escape_gap(60) = 180 -> keep.
def _recoverer():
    return ([(10., 100.)] + [(float(t), 100.) for t in range(20, 71, 10)]   # 60s plateau
            + [(80., 110.)]                                                  # escape 1
            + [(float(t), 110.) for t in range(90, 141, 10)]                 # 60s plateau
            + [(150., 120.)]                                                 # escape 2
            + [(float(t), 120.) for t in range(160, 311, 10)])               # current plateau ~160s


def test_recoverer_within_envelope_keeps():
    assert verdict(_recoverer(), 200.0) == "keep"


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


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main([__file__, "-q"]))
