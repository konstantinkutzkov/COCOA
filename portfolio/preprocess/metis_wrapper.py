"""Wrapper around the metis_features C++ helper.

Runs the binary as a subprocess and parses its `key=value`-per-line
output into a plain dict. Failure modes:

- Helper not found / not executable → MetisHelperMissing (the router
  should treat this as "no METIS features available" rather than crash).
- Helper exits with a non-zero status → MetisError, with the parsed
  output (if any) attached for diagnostics.
- Helper times out → MetisTimeout. Used by the router as a fallback
  signal (`route to ganak, this instance is too big for cheap structural
  analysis`).
"""

from __future__ import annotations

import os
import subprocess
from typing import Dict, Optional


class MetisHelperMissing(RuntimeError):
    pass


class MetisError(RuntimeError):
    def __init__(self, msg: str, partial: Optional[Dict[str, str]] = None):
        super().__init__(msg)
        self.partial = partial or {}


class MetisTimeout(RuntimeError):
    pass


def _helper_path() -> str:
    p = os.environ.get("PORTFOLIO_METIS_FEATURES_BIN")
    if p:
        return p
    here = os.path.dirname(os.path.abspath(__file__))
    default = os.path.normpath(os.path.join(
        here, "..", "..", "cocoa", "build", "metis_features"
    ))
    return default


def _parse_kv(text: str) -> Dict[str, str]:
    """Parse `key=value` lines into a dict. Last-write wins on duplicates."""
    out: Dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or "=" not in line:
            continue
        k, _, v = line.partition("=")
        out[k.strip()] = v.strip()
    return out


def _coerce(d: Dict[str, str]) -> Dict[str, object]:
    """Best-effort numeric coercion for known numeric keys.

    Strings stay strings; unknown keys are left as strings. Sentinel
    values (-1, -1.0) used by the helper for "not applicable" cases are
    passed through unchanged so the caller can detect them.
    """
    INT_KEYS = {
        "metis_n_vars", "metis_n_clauses", "metis_sepsize",
        "metis_n_left", "metis_n_right", "metis_n_sep",
        "metis_sep_vars", "metis_sep_clauses",
        "metis_left_vars", "metis_right_vars",
        "metis_ret",
    }
    FLOAT_KEYS = {"metis_balance", "metis_sep_ratio", "metis_wall_time_s"}

    coerced: Dict[str, object] = {}
    for k, v in d.items():
        if k in INT_KEYS:
            try:
                coerced[k] = int(v)
            except ValueError:
                coerced[k] = v
        elif k in FLOAT_KEYS:
            try:
                coerced[k] = float(v)
            except ValueError:
                coerced[k] = v
        else:
            coerced[k] = v
    return coerced


def run(cnf_path: str, timeout_s: float = 30.0) -> Dict[str, object]:
    """Run metis_features on cnf_path and return the parsed feature dict.

    Always includes `metis_status` (e.g., "ok", "too_small", "metis_failed");
    callers should branch on this before consuming numeric fields. Sentinel
    values (-1) are used for non-`ok` statuses.

    Raises MetisHelperMissing / MetisError / MetisTimeout per their
    docstrings.
    """
    bin_path = _helper_path()
    if not os.path.exists(bin_path) or not os.access(bin_path, os.X_OK):
        raise MetisHelperMissing(
            f"metis_features helper not found / not executable: {bin_path}. "
            f"Build it with: cmake --build cocoa/build --target metis_features"
        )

    try:
        proc = subprocess.run(
            [bin_path, cnf_path],
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout_s,
        )
    except subprocess.TimeoutExpired as e:
        raise MetisTimeout(
            f"metis_features exceeded {timeout_s}s on {cnf_path}"
        ) from e

    parsed = _coerce(_parse_kv(proc.stdout))

    if proc.returncode != 0:
        raise MetisError(
            f"metis_features exit={proc.returncode} on {cnf_path}: "
            f"{proc.stderr.strip()!r}",
            partial=parsed,
        )

    return parsed
