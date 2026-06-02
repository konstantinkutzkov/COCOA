"""Solver wrappers. Each module knows the binary path + flag profiles for
one solver and exposes a `run()` function with a uniform return shape."""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import tempfile
import time

from . import sharpsat_separator
from . import ganak_canonical

REGISTRY = {
    "sharpsat-separator": sharpsat_separator,
    "ganak-canonical":    ganak_canonical,
}


class SolverError(Exception):
    pass


class SolverResult:
    """Uniform result shape returned by every solver wrapper."""
    def __init__(self, exit_code: int, wall_time_s: float,
                 count: str | None, stdout: str, stderr: str,
                 peak_rss_bytes: int | None = None,
                 timed_out: bool = False):
        self.exit_code = exit_code
        self.wall_time_s = wall_time_s
        self.count = count          # the model count as a string (None on failure)
        self.stdout = stdout
        self.stderr = stderr
        self.peak_rss_bytes = peak_rss_bytes  # child peak RSS in bytes (None if unmeasured)
        self.timed_out = timed_out

    @property
    def ok(self) -> bool:
        return self.exit_code == 0 and self.count is not None


# ---------------------------------------------------------------------------
# Shared process launcher.
#
# Both solver wrappers route their subprocess through here so that timeout
# handling, output capture, and peak-memory measurement live in ONE place.
#
# Peak RSS is read from the child's own rusage via os.wait4 (so it is the
# high-water mark of THIS child specifically, not a process-wide aggregate).
# ru_maxrss is in bytes on macOS/BSD and in kilobytes on Linux — normalized
# to bytes here. If wait4 is unavailable on the platform, peak_rss_bytes is
# reported as None and everything else still works.
# ---------------------------------------------------------------------------
class LaunchResult:
    def __init__(self, exit_code, wall_time_s, stdout, stderr,
                 peak_rss_bytes, timed_out):
        self.exit_code = exit_code
        self.wall_time_s = wall_time_s
        self.stdout = stdout
        self.stderr = stderr
        self.peak_rss_bytes = peak_rss_bytes
        self.timed_out = timed_out


def launch(cmd: list[str], hard_timeout_s: float | None,
           poll_interval_s: float = 0.01) -> LaunchResult:
    """Run `cmd`, capturing stdout/stderr, wall time, and child peak RSS.

    `hard_timeout_s` is a wall-clock ceiling enforced by SIGKILL on the
    child's process group. None = no ceiling. Raises FileNotFoundError if
    the binary is not runnable (callers translate to SolverError).
    """
    out_f = tempfile.TemporaryFile()
    err_f = tempfile.TemporaryFile()
    t0 = time.monotonic()
    try:
        proc = subprocess.Popen(cmd, stdout=out_f, stderr=err_f,
                                 start_new_session=True)
    except BaseException:
        out_f.close()
        err_f.close()
        raise

    deadline = (t0 + hard_timeout_s) if hard_timeout_s else None
    timed_out = False
    rusage = None
    raw_status = 0

    have_wait4 = hasattr(os, "wait4")
    while True:
        if have_wait4:
            pid, raw_status, ru = os.wait4(proc.pid, os.WNOHANG)
        else:  # pragma: no cover - non-POSIX fallback
            pid = proc.poll()
            pid = proc.pid if pid is not None else 0
            ru = None
        if pid != 0:
            rusage = ru
            break
        if deadline is not None and time.monotonic() > deadline:
            _kill_group(proc.pid)
            timed_out = True
            if have_wait4:
                _, raw_status, rusage = os.wait4(proc.pid, 0)
            else:  # pragma: no cover
                proc.wait()
            break
        time.sleep(poll_interval_s)

    wall = time.monotonic() - t0

    # Resolve exit code. On timeout we report the conventional 124.
    if timed_out:
        exit_code = 124
    elif have_wait4:
        exit_code = os.waitstatus_to_exitcode(raw_status)
    else:  # pragma: no cover
        exit_code = proc.returncode if proc.returncode is not None else -1
    # Keep the Popen object consistent so its destructor does not re-reap.
    proc.returncode = exit_code if exit_code >= 0 else 1

    out_f.seek(0)
    err_f.seek(0)
    stdout = out_f.read().decode("utf-8", "replace")
    stderr = err_f.read().decode("utf-8", "replace")
    out_f.close()
    err_f.close()

    peak_rss_bytes = None
    if rusage is not None:
        maxrss = getattr(rusage, "ru_maxrss", 0)
        if maxrss:
            # Linux reports KB; macOS/BSD report bytes.
            peak_rss_bytes = maxrss if sys.platform == "darwin" else maxrss * 1024

    return LaunchResult(exit_code, wall, stdout, stderr,
                        peak_rss_bytes, timed_out)


def _kill_group(pid: int) -> None:
    try:
        os.killpg(os.getpgid(pid), signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def run(solver_name: str, profile: str, input_cnf: str,
        extra_flags: list[str] | None = None,
        time_budget_s: int | None = None) -> SolverResult:
    """Dispatch to the named solver's run() with the requested profile."""
    if solver_name not in REGISTRY:
        raise SolverError(f"unknown solver: {solver_name!r} "
                          f"(known: {list(REGISTRY)})")
    return REGISTRY[solver_name].run(
        profile=profile,
        input_cnf=input_cnf,
        extra_flags=extra_flags or [],
        time_budget_s=time_budget_s,
    )
