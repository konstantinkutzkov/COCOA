"""Managed solver process for the step-2 race: launch, live-parse progress, and
SIGSTOP / SIGCONT / SIGKILL the whole process group.

The point of SIGSTOP/SIGCONT (POSIX, identical on Linux and macOS — only the
signal *numbers* differ, so we always use the symbolic names) is RESUME: a frozen
process keeps its exact state, so a frontrunner continues instead of restarting,
and its earlier work is credited toward the budget.

We launch with start_new_session=True and signal the whole group, so any helper
threads/children freeze too. A SIGSTOP'd process is still alive, so proc.poll()
returns None while stopped — `finished()` therefore means "exited on its own".
"""
from __future__ import annotations

import os
import re
import signal
import subprocess
import math
import threading
import time

# --- progress / count parsers (formats verified against the real binaries) ---

# COCOA, on stderr: "PROGRESS t=.. pct_lin=.. closed_bits=.. decisions=.. l2_hits=.."
# COCOA count: bare big-int between "# solutions" and "# END"; "TIMEOUT !" = none.
# Ganak (--verb), on stdout: periodic "c o ..." rate lines; no completion fraction.
_GANAK_COUNT = re.compile(r"c s exact arb int\s+(\d+)")
_G_ENTRIES = re.compile(r"cache entries K\s+(\d+)")
_G_CONFL = re.compile(r"conflicts\s+(\d+)\b.*?confl/s:\s*([\d.]+)")
_G_CUBES = re.compile(r"cubes resolved/flp-removed\s+(\d+)")
_G_MISS = re.compile(r"cache miss rate\s+([\d.]+)")
_G_KLPS = re.compile(r"Klookup/s:\s*([\d.]+)")


def _kv(line: str) -> dict:
    out = {}
    for tok in line.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


class ManagedProc:
    """One archetype's process, with live progress + suspend/resume/kill."""

    def __init__(self, arch, cnf: str, progress_interval: float = 15.0):
        self.arch = arch
        self.cnf = cnf
        self.pi = progress_interval
        self.proc: subprocess.Popen | None = None
        self._lock = threading.Lock()
        self._sample: dict = {"engine": arch.engine}
        self._traj: list[tuple] = []   # (active_s, pct_lin, closed_bits) per PROGRESS (COCOA)
        self._count: str | None = None
        self._timed_out = False
        self._in_solutions = False
        self._sol_lines: list[str] = []
        self.stopped = False
        self._reader: threading.Thread | None = None
        # active (non-suspended) wall time accounting
        self._active_wall = 0.0
        self._t_resumed: float | None = None

    # --- lifecycle ---
    def start(self):
        self.proc = subprocess.Popen(
            self.arch.argv(self.cnf),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1, start_new_session=True,
            env=self.arch.env(self.pi),
        )
        self._t_resumed = time.monotonic()
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()

    def _pgid(self):
        return os.getpgid(self.proc.pid)

    def stop(self):
        """SIGSTOP the group (freeze)."""
        if self.stopped or self.proc is None or self.finished():
            return
        try:
            os.killpg(self._pgid(), signal.SIGSTOP)
        except (ProcessLookupError, PermissionError):
            return
        self.stopped = True
        if self._t_resumed is not None:
            self._active_wall += time.monotonic() - self._t_resumed
            self._t_resumed = None

    def cont(self):
        """SIGCONT the group (resume from exactly where it froze)."""
        if not self.stopped or self.proc is None:
            return
        try:
            os.killpg(self._pgid(), signal.SIGCONT)
        except (ProcessLookupError, PermissionError):
            return
        self.stopped = False
        self._t_resumed = time.monotonic()

    def kill(self):
        """SIGKILL the group and reap. SIGCONT first so a stopped group reaps."""
        if self.proc is None:
            return
        try:
            os.killpg(self._pgid(), signal.SIGCONT)
            os.killpg(self._pgid(), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        try:
            self.proc.wait(timeout=5)
        except (subprocess.TimeoutExpired, ValueError):
            pass
        if self._reader is not None:
            self._reader.join(timeout=1.0)

    # --- state queries ---
    def finished(self) -> bool:
        """Process exited on its own (NOT merely stopped)."""
        return self.proc is not None and self.proc.poll() is not None

    def drain(self, timeout: float = 2.0):
        """Wait for the reader to finish parsing buffered output. Call after the
        process has exited so a count line still in the pipe isn't missed (the
        reader's loop ends on EOF once the write end closes)."""
        if self._reader is not None:
            self._reader.join(timeout=timeout)

    def has_count(self) -> bool:
        with self._lock:
            return self._count is not None

    def count(self) -> str | None:
        with self._lock:
            return self._count

    def timed_out(self) -> bool:
        return self._timed_out

    def active_wall(self) -> float:
        w = self._active_wall
        if self._t_resumed is not None:
            w += time.monotonic() - self._t_resumed
        return w

    def latest(self) -> dict:
        with self._lock:
            return dict(self._sample)

    def closed_bits(self) -> float:
        """Current closed_bits LEVEL (how far along structurally). 0 if unknown."""
        with self._lock:
            try:
                return float(self._sample.get("closed_bits"))
            except (TypeError, ValueError):
                return 0.0

    def closed_bits_velocity(self) -> float:
        """RECENT closed_bits slope (bits/s) over the BACK HALF of the run, via
        least-squares — so a config progressing in stepwise bursts (plateau then
        jump, as observed in the t1_049 trajectory study) gets its true sustained
        rate, not a two-point slope that lands mid-plateau. Back half skips the BCP
        startup transient. 0 if too few samples.

        v = sum_{k in B}(t_k - t_bar)(c_k - c_bar) / sum_{k in B}(t_k - t_bar)^2,
        where B = samples with index >= floor(N/2) (the back half).
        """
        with self._lock:
            traj = list(self._traj)            # list of (active_s, pct_lin, closed_bits)
        n = len(traj)
        if n < 3:
            # too few for a back-half fit; fall back to a 2-point slope
            if n == 2 and traj[1][0] > traj[0][0]:
                return (traj[1][2] - traj[0][2]) / (traj[1][0] - traj[0][0])
            return 0.0
        back = traj[n // 2:]
        m = len(back)
        t_bar = sum(p[0] for p in back) / m
        c_bar = sum(p[2] for p in back) / m
        num = sum((p[0] - t_bar) * (p[2] - c_bar) for p in back)
        den = sum((p[0] - t_bar) ** 2 for p in back)
        return num / den if den > 0 else 0.0

    def pct_lin_traj(self) -> list:
        """[(active_seconds, pct_lin_percent)] — the OLD forecaster's input series."""
        with self._lock:
            return [(p[0], p[1]) for p in self._traj]

    def closed_bits_traj(self) -> list:
        """[(active_seconds, closed_bits)] — predict_cb's input series (well-conditioned;
        non-degenerate where pct_lin flatlines to ~0 on deep-tail instances)."""
        with self._lock:
            return [(p[0], p[2]) for p in self._traj]

    def n_root_estimate(self) -> float:
        """Target closed_bits where pct_lin=100%, for predict_cb. The solver doesn't emit
        it on PROGRESS lines, so recover it from pct_lin% = 100*2^(closed_bits - n_root):
        n_root = closed_bits - log2(pct/100), at the MAX-pct_lin sample (least relative
        noise in log2). inf if no positive-pct sample yet -> predict_cb yields ganak (safe)."""
        with self._lock:
            traj = list(self._traj)
        best = max(((p[1], p[2]) for p in traj if p[1] > 0.0), default=None)
        if best is None:
            return math.inf
        pct, cb = best
        return cb - math.log2(pct / 100.0)

    # --- reader ---
    def _read(self):
        try:
            for line in self.proc.stdout:  # type: ignore[union-attr]
                self._parse(line)
        except (ValueError, OSError):
            pass  # stdout closed during shutdown

    def _parse(self, line: str):
        if self.arch.engine == "cocoa":
            self._parse_cocoa(line)
        else:
            self._parse_ganak(line)

    def _parse_cocoa(self, line: str):
        if line.startswith("PROGRESS "):
            d = _kv(line)
            with self._lock:
                for k in ("t", "pct_lin", "closed_bits", "decisions", "l2_hits"):
                    if k in d:
                        self._sample[k] = d[k]
                # Record (ACTIVE time, pct_lin, closed_bits). We index by the
                # proc's active_wall(), NOT the solver's reported t, so SIGSTOP
                # freezes between rounds don't leave gaps/jumps on the time axis
                # the velocity + forecaster regress against.
                try:
                    self._traj.append((self.active_wall(),
                                       float(d["pct_lin"]), float(d["closed_bits"])))
                except (KeyError, ValueError):
                    pass
        elif "# END" in line:
            self._in_solutions = False
            with self._lock:
                txt = "".join(self._sol_lines).strip()
                if txt and self._count is None:
                    self._count = txt
        elif "# solutions" in line:
            self._in_solutions = True
        elif self._in_solutions:
            self._sol_lines.append(line)
        elif "TIMEOUT !" in line:
            self._timed_out = True

    def _parse_ganak(self, line: str):
        m = _GANAK_COUNT.search(line)
        if m:
            with self._lock:
                if self._count is None:
                    self._count = m.group(1)
            return
        with self._lock:
            mm = _G_ENTRIES.search(line)
            if mm:
                self._sample["cache_entries_k"] = mm.group(1)
            mm = _G_CONFL.search(line)
            if mm:
                self._sample["conflicts"] = mm.group(1)
                self._sample["confl_s"] = mm.group(2)
            mm = _G_CUBES.search(line)
            if mm:
                self._sample["cubes_resolved"] = mm.group(1)
            mm = _G_MISS.search(line)
            if mm:
                self._sample["cache_miss_rate"] = mm.group(1)
            mm = _G_KLPS.search(line)
            if mm:
                self._sample["klookup_s"] = mm.group(1)
