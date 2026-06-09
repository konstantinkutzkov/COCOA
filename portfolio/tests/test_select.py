"""Tests for the stage-aware funnel cut `_select`:
  stage 0 (8->4) closed_bits LEVEL; stage 1 (4->2) leader+fastest HEDGE; stage 2 (2->1) BANDED.

Stub procs use steady-rate trajectories, so predict_cb_recency's recency-weighted rate equals
the steady rate exactly -> eta = (NROOT - cb_end) / rate is predictable.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from race.scheduler import _select   # noqa: E402

NROOT = 105.0


class _Arch:
    def __init__(self, name):
        self.name = name
        self.engine = "cocoa"


class _Proc:
    """cb_end = final closed_bits; rate = steady b/s -> eta = (NROOT-cb_end)/rate."""
    def __init__(self, name, cb_end, rate, n=6, dt=15.0):
        self.arch = _Arch(name)
        self._traj = [(i * dt, cb_end - rate * (n - 1 - i) * dt) for i in range(n)]
        assert self._traj[0][1] > 0.0   # stay warm (no cb<=0 points dropped)

    def closed_bits(self):
        return self._traj[-1][1]

    def closed_bits_traj(self):
        return self._traj

    def n_root_estimate(self):
        return NROOT

    def active_wall(self):
        return self._traj[-1][0]


def _names(procs):
    return sorted(p.arch.name for p in procs)


def _dl():
    return time.monotonic() + 3600.0


def _nolog(*a, **k):
    pass


# 2->1, near-tie (|dcb| < 1.5): take the smaller ETA even though its cb is (barely) lower.
def test_banded_near_tie_takes_min_eta():
    A = _Proc("slow_close", cb_end=100.0, rate=0.005)   # cb 100, eta ~1000
    B = _Proc("fast_near",  cb_end=99.0,  rate=0.1)      # cb 99,  eta ~60  (dcb=1 < 1.5)
    assert _names(_select([A, B], 1, _dl(), _nolog, stage=2)) == ["fast_near"]


# 2->1, large gap (|dcb| >= 1.5): take the larger cb even though its ETA is far worse.
def test_banded_large_gap_takes_max_cb():
    A = _Proc("slow_ahead",  cb_end=100.0, rate=0.005)  # cb 100, eta ~1000
    C = _Proc("fast_behind", cb_end=95.0,  rate=0.1)     # cb 95,  eta ~100 (dcb=5 >= 1.5)
    assert _names(_select([A, C], 1, _dl(), _nolog, stage=2)) == ["slow_ahead"]


# 4->2 hedge: keep the cb-leader AND the (distinct) eta-leader.
def test_hedge_keeps_leader_and_fastest():
    A = _Proc("cb_leader",  cb_end=100.0, rate=0.005)    # max cb, eta ~1000
    B = _Proc("mid1",       cb_end=95.0,  rate=0.05)     # eta ~200
    C = _Proc("mid2",       cb_end=90.0,  rate=0.05)     # eta ~300
    D = _Proc("eta_leader", cb_end=85.0,  rate=0.15)     # min eta ~133
    assert _names(_select([A, B, C, D], 2, _dl(), _nolog, stage=1)) == ["cb_leader", "eta_leader"]


# 4->2 hedge tie: cb-leader IS also the eta-leader -> keep it + the 2nd-smallest ETA.
def test_hedge_tie_adds_second_eta():
    A = _Proc("both",    cb_end=100.0, rate=0.5)         # max cb AND min eta (~10)
    B = _Proc("mid",     cb_end=95.0,  rate=0.05)        # eta ~200
    C = _Proc("2nd_eta", cb_end=90.0,  rate=0.15)        # eta ~100  (2nd smallest)
    D = _Proc("slow",    cb_end=85.0,  rate=0.0667)      # eta ~300
    assert _names(_select([A, B, C, D], 2, _dl(), _nolog, stage=1)) == ["2nd_eta", "both"]


# 8->4 (stage 0): pure closed_bits LEVEL, ignore rate entirely.
def test_stage0_ranks_by_level():
    procs = [_Proc(f"cb{c}", cb_end=float(c), rate=0.05) for c in (100, 95, 90, 85)]
    assert _names(_select(procs, 2, _dl(), _nolog, stage=0)) == ["cb100", "cb95"]
