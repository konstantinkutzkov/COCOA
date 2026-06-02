"""Arjun reduction probe for solver selection — cheap, bounded TRIAGE only.

Runs the standalone `arjun` on a CNF and watches its running independent-support
size |I|, reported as `new size: N` in Arjun's `[arjun-simp]` stream (the final
value is echoed as `sampl: N`; cross-checkable against the `c p show` set in the
output). The probe's ONLY job is the routing decision (does Arjun shrink this
instance enough that Ganak's projected counting is the right tool). The
simplified formula is discarded: Ganak re-runs its own Arjun if we route there
(its parser/counter can't ingest a pre-Arjun'd CNF — verified 2026-06-02). So we
never keep output.

Control: monitor |I| (monotone non-increasing) against the ORIGINAL n, with four
stop triggers:
  - SUCCESS  : |I|/n <= success_indep_frac        -> stop early (it reduced enough)
  - STALL    : |I| hasn't dropped for `stall_s`    -> stop (not reducing)
  - CAP      : total wall >= `cap_s`               -> stop (backstop)
  - FINISHED : Arjun completed on its own
The verdict (Ganak vs UNDECIDED) is taken by select_solver from the achieved
|I|/n; `stop_reason` is recorded for telemetry.
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


# Running independent-support size |I|, e.g.
#   [arjun-simp] Removed set: 139 new size: 117    (final value echoed as `sampl: 117`)
# ANSI color codes may surround the token; the digits follow the colon directly.
_RE_INDEP = re.compile(r"(?:new size|sampl):\s*(\d+)")


def _helper_path() -> str:
    p = os.environ.get("PORTFOLIO_ARJUN_BIN")
    if p:
        return p
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(
        here, "..", "..", "ganak-canonical", "build", "_deps",
        "arjun-build", "arjun"))


def _read_nvars(path: str) -> Optional[int]:
    try:
        with open(path) as f:
            for line in f:
                if line.startswith("p cnf"):
                    return int(line.split()[2])
    except (OSError, ValueError, IndexError):
        pass
    return None


def run(cnf_path: str, stall_s: float = 5.0, cap_s: float = 60.0,
        success_indep_frac: float = 0.50, poll_s: float = 0.05) -> dict:
    """Probe Arjun on cnf_path; return reduction signals + stop_reason.

    Keys: orig_vars(n), indep_size(min |I| seen), indep_frac(|I|/n),
          trajectory[(t, |I|)], stop_reason in
          {success,stall,cap,finished,no_report,no_header}, wall_s.
    """
    bin_path = _helper_path()
    if not os.path.exists(bin_path) or not os.access(bin_path, os.X_OK):
        raise ArjunHelperMissing(
            f"arjun helper not found / not executable: {bin_path} "
            f"(build ganak-canonical, or set PORTFOLIO_ARJUN_BIN)")

    n = _read_nvars(cnf_path)
    if n is None:
        # Unparseable `p cnf` header: no baseline to measure |I|/n against.
        # Degrade to a null result (select_solver treats indep_frac=None as
        # not-substantial -> UNDECIDED) without spending the stall window.
        return {"orig_vars": None, "indep_size": None, "indep_frac": None,
                "trajectory": [], "stop_reason": "no_header", "wall_s": 0.0}

    # Arjun wants an output path arg to run its full pipeline; we discard it.
    fd, scratch = tempfile.mkstemp(suffix=".arjun_discard.cnf")
    os.close(fd)
    try:
        return _probe(bin_path, cnf_path, scratch, n,
                      stall_s, cap_s, success_indep_frac, poll_s)
    finally:
        try:
            os.unlink(scratch)
        except OSError:
            pass


def _probe(bin_path: str, cnf_path: str, scratch: str, n: int,
           stall_s: float, cap_s: float, success_indep_frac: float,
           poll_s: float) -> dict:
    state = {
        "free": n,            # current |I|
        "min_free": n,        # smallest |I| seen
        "last_drop_t": 0.0,   # wall (rel t0) of last decrease / first report
        "traj": [],           # [(t, |I|)]
    }
    lock = threading.Lock()
    t0 = time.monotonic()

    proc = subprocess.Popen(
        [bin_path, cnf_path, scratch],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1, start_new_session=True,
    )

    def _reader():
        try:
            for line in proc.stdout:  # type: ignore[union-attr]
                m = _RE_INDEP.search(line)
                if not m:
                    continue
                fv = int(m.group(1))
                t = time.monotonic() - t0
                with lock:
                    first_report = not state["traj"]
                    state["traj"].append((round(t, 3), fv))
                    state["free"] = fv
                    # None-check first so we never evaluate `fv < None`.
                    is_drop = state["min_free"] is None or fv < state["min_free"]
                    if is_drop:
                        state["min_free"] = fv
                    # Anchor the stall clock on the first report OR any strict
                    # drop, so Arjun's startup latency before its first
                    # [arjun-simp] line can't trigger an immediate STALL.
                    if first_report or is_drop:
                        state["last_drop_t"] = t
        except (ValueError, OSError):
            pass  # stdout closed during shutdown

    th = threading.Thread(target=_reader, daemon=True)
    th.start()

    stop_reason = None
    while True:
        if proc.poll() is not None:
            stop_reason = "finished"
            break
        now = time.monotonic() - t0
        with lock:
            mf = state["min_free"]
            last_drop = state["last_drop_t"]
            seen = bool(state["traj"])
        # SUCCESS: reduced enough already (monotone => it only stays/improves)
        if n and mf is not None and seen and (mf / n) <= success_indep_frac:
            stop_reason = "success"
            break
        # STALL: |I| hasn't dropped in stall_s (measured from the first report)
        if seen and (now - last_drop) >= stall_s:
            stop_reason = "stall"
            break
        # CAP: hard wall backstop
        if now >= cap_s:
            stop_reason = "cap"
            break
        time.sleep(poll_s)

    if stop_reason != "finished":
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
    proc.wait()
    try:
        if proc.stdout:
            proc.stdout.close()   # force EOF so the reader drains and exits
    except OSError:
        pass
    th.join(timeout=2.0)
    wall = round(time.monotonic() - t0, 3)

    with lock:
        traj = list(state["traj"])
        min_free = state["min_free"]
    if not traj:
        stop_reason = "no_report"
        min_free = n
    indep_frac = (min_free / n) if (n and min_free is not None) else None

    return {
        "orig_vars": n,
        "indep_size": min_free,
        "indep_frac": indep_frac,
        "trajectory": traj,
        "stop_reason": stop_reason,
        "wall_s": wall,
    }
