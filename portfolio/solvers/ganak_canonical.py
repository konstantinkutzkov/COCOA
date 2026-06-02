"""Wrapper around the ganak-canonical fork.

The fork (ganak + the `--cachehash canonical` port) has landed, so the
`canonical` profile is live. PORTFOLIO_GANAK_BIN must point at the fork
binary (ganak-canonical/build/ganak); vanilla ganak does not understand
`--cachehash` and will reject the canonical/identity profiles.

A/B profiles mirror sharpsat-separator: `identity` selects ganak's cheap
native hash explicitly, `canonical` selects the anonymized WL key, so a
comparison isolates the hashing decision.
"""

from __future__ import annotations

import os
import re
from typing import List, Optional


PROFILES: dict[str, List[str]] = {
    # Router default — ganak's own defaults (native cache hash).
    "default": [],

    # --- A/B versions (see portfolio/versions.py) ---
    # Identity (fast) baseline: ganak's native packed hash, made explicit
    # so it is parallel to the canonical profile.
    "identity":  ["--cachehash", "hashed"],
    # Anonymized (powerful) extension: the ported WL-refined canonical key.
    "canonical": ["--cachehash", "canonical", "--wliter", "2"],
}


# Ganak count output lines (verified against ganak --help / sample output):
#   s SATISFIABLE
#   c s type mc
#   c s log10-estimate <x>
#   c s exact arb int <N>
_COUNT_LINE_RE = re.compile(
    r"^c?\s*s\s+exact(?:\s+arb)?(?:\s+\w+)?\s+([0-9eE+.\-/]+)\s*$",
    re.MULTILINE,
)


def _bin_path() -> str:
    p = os.environ.get("PORTFOLIO_GANAK_BIN")
    if not p:
        raise RuntimeError("PORTFOLIO_GANAK_BIN env var not set "
                           "(run via portfolio/run.sh)")
    return p


def _extract_count(stdout: str) -> Optional[str]:
    m = _COUNT_LINE_RE.search(stdout)
    return m.group(1) if m else None


def run(profile: str, input_cnf: str, extra_flags: List[str],
        time_budget_s: Optional[int]):
    from . import SolverResult, SolverError, launch  # local import to avoid cycle
    if profile not in PROFILES:
        raise SolverError(
            f"ganak-canonical: unknown profile {profile!r} "
            f"(known: {list(PROFILES)})")

    cmd = [_bin_path()] + PROFILES[profile]
    cmd += list(extra_flags)
    cmd += [input_cnf]

    # Ganak has no built-in wall-time-budget flag; the launcher enforces
    # the ceiling via SIGKILL and reports timed_out / exit 124.
    try:
        lr = launch(cmd, hard_timeout_s=time_budget_s)
    except FileNotFoundError as e:
        raise SolverError(f"ganak binary not runnable: {e}") from e

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
