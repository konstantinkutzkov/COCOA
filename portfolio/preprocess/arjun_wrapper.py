"""Time-budgeted Arjun reduction probe for the solver-selection router.

Runs the standalone `arjun` binary on a CNF under a WALL-CLOCK budget, and
MONITORS its reduction progress live (Arjun streams `c o Reduced to <V> vars,
<C> cls` lines throughout its strategy phases). Returns the reduction
trajectory plus, if Arjun finishes in budget, the final independent-support
size |I| (from the `c p show` line of the simplified CNF) and multiplier.

Why time-bounded + streamed (not "fixed timeout then read the end"): a barely-
reducing Arjun is visible early as a FLAT trajectory, so the router can tell
"this is not a Ganak instance" without trusting a single end-state — and on a
big formula Arjun can otherwise spin for minutes.

Failure modes:
- binary missing/not executable -> ArjunHelperMissing
- budget exceeded             -> result with timed_out=True (trajectory still usable)
"""

from __future__ import annotations

import os
import re
import signal
import subprocess
import tempfile
import threading
import time
from typing import Optional


class ArjunHelperMissing(RuntimeError):
    pass


_RE_REDUCED = re.compile(r"Reduced to\s+(\d+)\s+vars,\s+(\d+)\s+cls")
_RE_FINAL = re.compile(r"final vars:\s*(\d+)\s+final cls:\s*(\d+)")
_RE_PSHOW = re.compile(r"^c p show\s+(.*)$")
_RE_MULT = re.compile(r"MUST MULTIPLY BY\s+(\S+)")


def _helper_path() -> str:
    p = os.environ.get("PORTFOLIO_ARJUN_BIN")
    if p:
        return p
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(
        here, "..", "..", "ganak-canonical", "build", "_deps",
        "arjun-build", "arjun"))


def _read_header(path: str) -> tuple[Optional[int], Optional[int]]:
    """Return (nvars, nclauses) from a DIMACS `p cnf` header, or (None, None)."""
    try:
        with open(path) as f:
            for line in f:
                if line.startswith("p cnf"):
                    parts = line.split()
                    return int(parts[2]), int(parts[3])
    except (OSError, ValueError, IndexError):
        pass
    return None, None


def run(cnf_path: str, time_budget_s: float = 30.0,
        poll_s: float = 0.05) -> dict:
    """Probe Arjun on cnf_path under a wall budget; return reduction signals.

    Result dict keys:
      orig_vars, orig_cls           : input CNF size
      best_vars, best_cls           : smallest (vars, cls) observed (trajectory + final)
      simp_vars, simp_cls           : simplified CNF size (only if completed)
      indep_size, multiplier        : |I| and count multiplier (only if completed)
      trajectory                    : list of (elapsed_s, vars, cls) observations
      wall_s, completed, timed_out
    """
    bin_path = _helper_path()
    if not os.path.exists(bin_path) or not os.access(bin_path, os.X_OK):
        raise ArjunHelperMissing(
            f"arjun helper not found / not executable: {bin_path} "
            f"(build ganak-canonical, or set PORTFOLIO_ARJUN_BIN)")

    orig_vars, orig_cls = _read_header(cnf_path)
    fd, out_cnf = tempfile.mkstemp(suffix=".arjsimp.cnf")
    os.close(fd)

    traj: list[tuple[float, int, int]] = []
    t0 = time.monotonic()
    proc = subprocess.Popen(
        [bin_path, cnf_path, out_cnf],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1, start_new_session=True,
    )

    def _reader():
        # Timestamp each reduction observation with our own wall clock so the
        # trajectory has a real time axis (Arjun's own "T:" is not on every line).
        for line in proc.stdout:  # type: ignore[union-attr]
            m = _RE_REDUCED.search(line) or _RE_FINAL.search(line)
            if m:
                traj.append((round(time.monotonic() - t0, 3),
                             int(m.group(1)), int(m.group(2))))

    th = threading.Thread(target=_reader, daemon=True)
    th.start()

    timed_out = False
    deadline = t0 + time_budget_s
    while proc.poll() is None:
        if time.monotonic() >= deadline:
            timed_out = True
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
            break
        time.sleep(poll_s)
    proc.wait()
    th.join(timeout=1.0)
    wall = round(time.monotonic() - t0, 3)

    completed = (not timed_out) and proc.returncode == 0
    simp_vars = simp_cls = indep_size = None
    multiplier = None
    if completed and os.path.exists(out_cnf):
        simp_vars, simp_cls = _read_header(out_cnf)
        try:
            with open(out_cnf) as f:
                for line in f:
                    mp = _RE_PSHOW.match(line)
                    if mp:
                        toks = [t for t in mp.group(1).split() if t and t != "0"]
                        indep_size = len(toks)
                    mm = _RE_MULT.search(line)
                    if mm:
                        multiplier = mm.group(1)
        except OSError:
            pass
    try:
        os.unlink(out_cnf)
    except OSError:
        pass

    best_vars = min([v for _, v, _ in traj], default=orig_vars if orig_vars else 0)
    best_cls = min([c for _, _, c in traj], default=orig_cls if orig_cls else 0)
    if simp_vars is not None:
        best_vars = min(best_vars, simp_vars)
    if simp_cls is not None:
        best_cls = min(best_cls, simp_cls)

    return {
        "orig_vars": orig_vars, "orig_cls": orig_cls,
        "best_vars": best_vars, "best_cls": best_cls,
        "simp_vars": simp_vars, "simp_cls": simp_cls,
        "indep_size": indep_size, "multiplier": multiplier,
        "trajectory": traj, "wall_s": wall,
        "completed": completed, "timed_out": timed_out,
    }
