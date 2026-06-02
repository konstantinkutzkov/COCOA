"""Wrapper around our solver (sharpsat-separator) — the COCOA fork.

Profiles are stable contracts with the router; the flag set behind a
profile may evolve. Adding a new profile is a one-place change here.

A/B profiles `identity` / `canonical` share an IDENTICAL base flag set and
differ only in the cache-hash mode, so a comparison between them isolates
the hashing decision. COCOA's binary defaults to canonical hashing, so the
`identity` profile sets `-hashMode identity` explicitly.
"""

from __future__ import annotations

import os
import re
from typing import List, Optional


# Base flags shared by the A/B profiles. Kept identical across identity /
# canonical so the only varying dimension is the hash. (Arjun is gone from
# COCOA, so -cb is safe again — see no_cb_on_arjun feedback, now moot here.)
_BASE = ["-rec", "-sep", "5", "-cb", "3"]

# Flag profiles. Names are stable contracts with the router.
PROFILES: dict[str, List[str]] = {
    # Router default — kept for backwards compatibility with classifier
    # decisions (which request profile="default"). Does not pin -hashMode,
    # so it uses the binary's default (canonical).
    "default": ["-rec", "-sep", "5", "-cb", "0", "-metisVars", "-wlIter", "2"],

    # --- A/B versions (see portfolio/versions.py) ---
    # Identity (fast) baseline: cheap chibihash64, literal-identity matches only.
    "identity":  _BASE + ["-hashMode", "identity"],
    # Anonymized (powerful) extension: WL-refined isomorphism-aware key.
    "canonical": _BASE + ["-hashMode", "canonical", "-wlIter", "2"],
}


# sharpSAT prints the count delimited by `# solutions` / `# END`:
#   # solutions
#   132951278067432
#   # END
# Capture the integer between those two markers.
_SOLUTIONS_BLOCK_RE = re.compile(
    r"^#\s*solutions\s*\n([0-9]+)\s*\n#\s*END",
    re.MULTILINE,
)


def _bin_path() -> str:
    p = os.environ.get("PORTFOLIO_SHARPSAT_BIN")
    if not p:
        raise RuntimeError("PORTFOLIO_SHARPSAT_BIN env var not set "
                           "(run via portfolio/run.sh)")
    return p


def _extract_count(stdout: str) -> Optional[str]:
    """Parse the model count from sharpSAT's stdout. Returns None if absent."""
    m = _SOLUTIONS_BLOCK_RE.search(stdout)
    return m.group(1) if m else None


def run(profile: str, input_cnf: str, extra_flags: List[str],
        time_budget_s: Optional[int]):
    from . import SolverResult, SolverError, launch  # local import to avoid cycle
    if profile not in PROFILES:
        raise SolverError(
            f"sharpsat-separator: unknown profile {profile!r} "
            f"(known: {list(PROFILES)})")
    cmd = [_bin_path()] + PROFILES[profile]
    if time_budget_s is not None:
        # sharpSAT honours its own `-t` budget (graceful internal stop).
        cmd += ["-t", str(time_budget_s)]
    cmd += list(extra_flags)
    cmd += [input_cnf]

    # Hard ceiling a little above the solver's own -t, enforced by the
    # launcher via SIGKILL on the process group.
    hard = (time_budget_s + 5) if time_budget_s is not None else None
    try:
        lr = launch(cmd, hard_timeout_s=hard)
    except FileNotFoundError as e:
        raise SolverError(f"sharpsat-separator binary not runnable: {e}") from e

    count = _extract_count(lr.stdout) if lr.exit_code == 0 else None
    return SolverResult(
        exit_code=lr.exit_code,
        wall_time_s=lr.wall_time_s,
        count=count,
        stdout=lr.stdout,
        stderr=lr.stderr,
        peak_rss_bytes=lr.peak_rss_bytes,
        timed_out=lr.timed_out,
    )
