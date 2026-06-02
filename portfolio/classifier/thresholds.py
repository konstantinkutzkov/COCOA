"""Hand-tuned router (v2 — first real decision logic).

Decision rule mirrors the Phase-2 acceptance gate our own solver uses to
decide whether its precomputed ND-hierarchy separator is good enough
(separator_max_ratio = 0.20, separator_min_balance = 0.30 — see
sharpsat-separator/src/solver_config.h). If METIS reports a separator
that would have passed THAT gate, this is the kind of instance our
solver is designed for; route to it. Otherwise fall through to ganak.

Tiers of decisions, in order:

1. If METIS features are present AND the separator passes the Phase-2
   gate → ROUTE TO OUR SOLVER.
2. Otherwise (METIS too-small, missing, helper-error, or sep too big /
   too imbalanced) → ROUTE TO GANAK.

This is intentionally simple. Cases where it routes wrong become test
cases in tests/test_route.py and training data for a learned classifier
later (classifier/predict.py, Commit 5+).
"""

from __future__ import annotations

from dataclasses import dataclass


CLASSIFIER_VERSION = "thresholds-v2-phase2gate"


# Gate thresholds — keep in sync with sharpsat-separator's
# separator_max_ratio / separator_min_balance defaults.
SEP_RATIO_MAX = 0.20
BALANCE_MIN   = 0.30


@dataclass
class Decision:
    solver: str
    profile: str
    classifier_version: str
    reason: str

    def to_dict(self):
        return {"solver": self.solver,
                "profile": self.profile,
                "classifier_version": self.classifier_version,
                "reason": self.reason}


def decide(features) -> Decision:
    """Pick a solver + profile based on the extracted features.

    The `reason` field is a short human-readable tag that lands in
    decisions.jsonl so we can audit routing post-hoc without re-deriving
    the decision logic from the feature record.
    """
    sep_ratio = features.metis_sep_ratio
    balance   = features.metis_root_balance

    if sep_ratio is None or balance is None:
        return Decision(
            solver="ganak-canonical",
            profile="default",
            classifier_version=CLASSIFIER_VERSION,
            reason="metis_features_missing",
        )

    if sep_ratio <= SEP_RATIO_MAX and balance >= BALANCE_MIN:
        return Decision(
            solver="sharpsat-separator",
            profile="default",
            classifier_version=CLASSIFIER_VERSION,
            reason=f"phase2_gate_pass(sep_ratio={sep_ratio:.3f}, balance={balance:.3f})",
        )

    return Decision(
        solver="ganak-canonical",
        profile="default",
        classifier_version=CLASSIFIER_VERSION,
        reason=f"phase2_gate_fail(sep_ratio={sep_ratio:.3f}, balance={balance:.3f})",
    )
