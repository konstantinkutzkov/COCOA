"""Wrapper around the nd_cost C++ helper.

Runs cocoa/build/nd_cost on a CNF and parses its `key=value`-per-line output
into a dict. nd_cost builds COCOA's full nested-dissection hierarchy (the exact
recursive METIS bisection the solver uses) and reports the decomposition's
branching cost `nd_log2_cost` = L(root) in bits (log2 of estimated branch-leaves,
ignoring caching/BCP). This is the whole-ND-tree router signal that replaces the
single-root-cut sep_ratio.

PROVISIONAL: the absolute bits overestimate real COCOA work (caching is ignored),
so the cost is reliable only as a HIGH-END filter today — see select_solver.py.

Failure modes mirror metis_wrapper:
- helper not found / not executable -> NdCostHelperMissing
- non-zero exit                     -> NdCostError (parsed output attached)
- timeout                           -> NdCostTimeout
"""
from __future__ import annotations

import os
import subprocess
from typing import Dict, Optional


class NdCostHelperMissing(RuntimeError):
    pass


class NdCostError(RuntimeError):
    def __init__(self, msg: str, partial: Optional[Dict[str, object]] = None):
        super().__init__(msg)
        self.partial = partial or {}


class NdCostTimeout(RuntimeError):
    pass


def _helper_path() -> str:
    p = os.environ.get("PORTFOLIO_NDCOST_BIN")
    if p:
        return p
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "..", "cocoa", "build", "nd_cost"))


_INT_KEYS = {
    "nd_n_vars", "nd_n_clauses", "nd_n_constrained_vars", "nd_vars_only",
    "nd_root_sep_vars", "nd_worst_leaf_vars", "nd_n_nodes", "nd_n_internal",
    "nd_n_passthrough", "nd_n_leaves", "nd_total_sep_vars", "nd_total_sep_clauses",
    "nd_acct_ok", "nd_acct_sum",
}
_FLOAT_KEYS = {"nd_log2_cost", "nd_max_path_sep", "nd_wall_time_s"}


def _coerce(d: Dict[str, str]) -> Dict[str, object]:
    out: Dict[str, object] = {}
    for k, v in d.items():
        if k in _INT_KEYS:
            try:
                out[k] = int(v)
            except ValueError:
                out[k] = v
        elif k in _FLOAT_KEYS:
            try:
                out[k] = float(v)
            except ValueError:
                out[k] = v
        else:
            out[k] = v
    return out


def run(cnf_path: str, timeout_s: float = 120.0) -> Dict[str, object]:
    """Run nd_cost on cnf_path; return the parsed feature dict.

    Always includes `nd_status` (e.g. "ok", "too_small", "build_failed");
    callers should branch on it before consuming numeric fields.
    """
    bin_path = _helper_path()
    if not os.path.exists(bin_path) or not os.access(bin_path, os.X_OK):
        raise NdCostHelperMissing(
            f"nd_cost helper not found / not executable: {bin_path}. "
            f"Build it with: cmake --build cocoa/build --target nd_cost"
        )
    try:
        proc = subprocess.run([bin_path, cnf_path], capture_output=True,
                              text=True, check=False, timeout=timeout_s)
    except subprocess.TimeoutExpired as e:
        raise NdCostTimeout(f"nd_cost exceeded {timeout_s}s on {cnf_path}") from e

    parsed: Dict[str, str] = {}
    for line in proc.stdout.splitlines():
        line = line.strip()
        if "=" in line:
            k, _, v = line.partition("=")
            parsed[k.strip()] = v.strip()
    coerced = _coerce(parsed)

    if proc.returncode != 0:
        raise NdCostError(
            f"nd_cost exit={proc.returncode} on {cnf_path}: {proc.stderr.strip()!r}",
            partial=coerced,
        )
    return coerced
