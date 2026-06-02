"""Cheap structural feature extraction for routing decisions.

Cost tiers:
- Tier 1 (parse-only): n_vars, n_clauses, density, clause stats, indep set
  marker. Microseconds-to-milliseconds.
- Tier 2 (METIS via tools/metis_features): root separator size, balance,
  sep_ratio. Single-digit seconds even on big instances.

Schema is stable; new features can be added but existing keys never
change meaning or get renamed (downstream classifier compatibility).
"""

from __future__ import annotations

from dataclasses import dataclass, asdict
from typing import Dict, Any, Optional


@dataclass
class Features:
    """Per-instance feature record.

    Fields populated by features.extract() at the cost-tier they belong to.
    Missing higher-cost fields stay None so the router can decide whether
    to gather more before deciding.
    """
    # Cheap (cnf-parse) features:
    n_vars: int = 0
    n_clauses: int = 0
    primal_density: float = 0.0           # n_clauses / n_vars
    avg_clause_len: float = 0.0
    max_clause_len: int = 0
    n_binary_clauses: int = 0
    n_unit_clauses: int = 0
    indep_set_size: Optional[int] = None  # parsed from `c p show ... 0` if present

    # METIS features (populated in Commit 2):
    metis_root_sep_size: Optional[int] = None
    metis_root_balance: Optional[float] = None
    metis_sep_ratio: Optional[float] = None  # sep_size / n_active_vars

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)


def extract(cnf_path: str, with_metis: bool = True,
            metis_timeout_s: float = 30.0) -> Features:
    """Parse the CNF and compute features.

    Always runs Tier 1 (parse-only). Tier 2 (METIS) runs when with_metis
    is True (default); on any METIS failure (helper missing, timeout,
    crash) the METIS fields stay None and Tier-1 features are still
    returned. Callers that absolutely need METIS data should check
    `features.metis_root_sep_size is not None` after extract.
    """
    f = Features()
    clause_lens = []
    indep_vars = None

    with open(cnf_path, "r") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("c"):
                # Look for `c p show <var1> <var2> ... 0` (sampling-set marker).
                # Multiple `c p show` lines may concatenate; we union them.
                tokens = line.split()
                if len(tokens) >= 4 and tokens[0] == "c" and tokens[1] == "p" and tokens[2] == "show":
                    if indep_vars is None:
                        indep_vars = set()
                    for tok in tokens[3:]:
                        try:
                            v = int(tok)
                        except ValueError:
                            break
                        if v == 0:
                            break
                        indep_vars.add(v)
                continue
            if line.startswith("p"):
                parts = line.split()
                # `p cnf <n_vars> <n_clauses>`
                if len(parts) >= 4 and parts[1] == "cnf":
                    f.n_vars = int(parts[2])
                    f.n_clauses = int(parts[3])
                continue
            # Clause line: literals terminated by 0
            lits = []
            for tok in line.split():
                try:
                    v = int(tok)
                except ValueError:
                    break
                if v == 0:
                    break
                lits.append(v)
            if lits:
                clause_lens.append(len(lits))

    if clause_lens:
        f.avg_clause_len = sum(clause_lens) / len(clause_lens)
        f.max_clause_len = max(clause_lens)
        f.n_binary_clauses = sum(1 for L in clause_lens if L == 2)
        f.n_unit_clauses = sum(1 for L in clause_lens if L == 1)

    if f.n_vars > 0:
        f.primal_density = f.n_clauses / f.n_vars

    if indep_vars is not None:
        f.indep_set_size = len(indep_vars)

    if with_metis:
        _populate_metis(f, cnf_path, timeout_s=metis_timeout_s)

    return f


def _populate_metis(f: Features, cnf_path: str, timeout_s: float) -> None:
    """Best-effort METIS feature population.

    Any error (helper missing, timeout, crash) is swallowed — METIS
    features stay None and Tier-1 features remain usable. The router
    decides what to do when METIS data is absent.
    """
    # Local import so Tier 1 is importable even if preprocess/ is broken.
    from preprocess import metis_wrapper

    try:
        d = metis_wrapper.run(cnf_path, timeout_s=timeout_s)
    except (metis_wrapper.MetisHelperMissing,
            metis_wrapper.MetisError,
            metis_wrapper.MetisTimeout):
        return

    if d.get("metis_status") != "ok":
        # too_small / metis_failed / etc. — leave Tier 2 fields None.
        return

    sep_vars = d.get("metis_sep_vars")
    balance  = d.get("metis_balance")
    sep_rat  = d.get("metis_sep_ratio")
    # All three are positive (non-sentinel) when status == "ok".
    if isinstance(sep_vars, int) and sep_vars >= 0:
        f.metis_root_sep_size = sep_vars
    if isinstance(balance, float) and balance >= 0:
        f.metis_root_balance = balance
    if isinstance(sep_rat, float) and sep_rat >= 0:
        f.metis_sep_ratio = sep_rat
