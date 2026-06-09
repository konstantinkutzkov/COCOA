"""Tests for the wall-clock watchdog (_spawn_wall_watchdog).

The main poll loop's `now >= deadline` check can be starved past the deadline on a loaded /
memory-thrashing machine (mc2026_027 ran ~6-9 min over a 60-min budget, never returning
BUDGET). The watchdog enforces the WALL cap independently: `grace_s` after the deadline it
force-kills every still-live spawned proc. These pin that contract with tiny timings (no
real subprocesses) so it stays fast.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from race.scheduler import _spawn_wall_watchdog   # noqa: E402


class _StubProc:
    """Minimal stand-in for ManagedProc exposing only what the watchdog touches."""
    def __init__(self, name, finished=False):
        self.proc = object()            # truthy -> "spawned"
        self.arch = type("A", (), {"name": name})()
        self._finished = finished
        self.killed = 0

    def finished(self):
        return self._finished

    def kill(self):
        self.killed += 1

    def active_wall(self):
        return 42.0


def _wait(cond, timeout=2.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.02)
    return cond()


# 1. A live proc still running past deadline+grace is force-killed.
def test_watchdog_force_kills_live_proc_after_grace():
    p = _StubProc("leader")
    _spawn_wall_watchdog(time.monotonic(), [p], budget_s=1.0, log=lambda *_: None,
                         grace_s=0.1, poll_s=0.02)
    assert _wait(lambda: p.killed >= 1)


# 2. If the run already finished (no live procs), the watchdog is a no-op.
def test_watchdog_noop_when_already_finished():
    p = _StubProc("leader", finished=True)
    _spawn_wall_watchdog(time.monotonic(), [p], budget_s=1.0, log=lambda *_: None,
                         grace_s=0.1, poll_s=0.02)
    time.sleep(0.25)   # well past deadline+grace
    assert p.killed == 0


# 3. It does NOT fire before the deadline+grace has elapsed (no premature kill).
def test_watchdog_does_not_fire_early():
    p = _StubProc("leader")
    _spawn_wall_watchdog(time.monotonic() + 5.0, [p], budget_s=10.0, log=lambda *_: None,
                         grace_s=0.1, poll_s=0.02)
    time.sleep(0.25)
    assert p.killed == 0


# 4. Empty proc list must not crash (max() over no live procs is never reached).
def test_watchdog_empty_list_no_crash():
    _spawn_wall_watchdog(time.monotonic(), [], budget_s=1.0, log=lambda *_: None,
                         grace_s=0.1, poll_s=0.02)
    time.sleep(0.25)
