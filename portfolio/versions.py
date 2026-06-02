"""Comparable solver versions for A/B testing.

A *version* is one point in the portfolio configuration space, expressed as
the two-stage choice the design calls for:

    1. select a solver   -> `solver`  (a key in solvers.REGISTRY)
    2. select its flags   -> `profile` (a key in that solver's PROFILES)

The primary A/B axis is the cache-hash mode:

    identity  — cheap chibihash64 / native packed hash; matches only
                LITERALLY identical components. Fast per call.
    canonical — WL-refinement isomorphism-aware ("anonymized") key; collapses
                structurally-equivalent components. ~1000x slower per call but
                far more cache hits on symmetric structure.

The base flags are aligned across the two solvers so that a comparison
isolates the solver and/or the hash rather than incidental flag differences.

The two BASIC versions are the identity baselines (one per solver); each
EXTENDS to its canonical variant by toggling only the hash. Adding richer
configurations later means adding entries here + a profile in the wrapper —
the A/B runner and any future router consume this table by name only.
"""

from __future__ import annotations

VERSIONS: dict[str, dict] = {
    "cocoa-identity": {
        "solver":  "sharpsat-separator",
        "profile": "identity",
        "hash":    "identity",
        "basic":   True,
        "desc":    "COCOA, identity hash (fast baseline)",
    },
    "cocoa-canonical": {
        "solver":  "sharpsat-separator",
        "profile": "canonical",
        "hash":    "canonical",
        "basic":   False,
        "desc":    "COCOA, anonymized (WL canonical) hash",
    },
    "ganak-identity": {
        "solver":  "ganak-canonical",
        "profile": "identity",
        "hash":    "identity",
        "basic":   True,
        "desc":    "Ganak, native hash (fast baseline)",
    },
    "ganak-canonical": {
        "solver":  "ganak-canonical",
        "profile": "canonical",
        "hash":    "canonical",
        "basic":   False,
        "desc":    "Ganak, anonymized (WL canonical) hash",
    },
}

# The two basic versions used for the primary A/B comparison.
BASIC = tuple(name for name, v in VERSIONS.items() if v["basic"])

# Each basic version paired with its canonical extension. Useful for the
# "does anonymization earn its per-call cost?" within-solver comparison.
HASH_PAIRS = (
    ("cocoa-identity", "cocoa-canonical"),
    ("ganak-identity", "ganak-canonical"),
)


def resolve(name: str) -> dict:
    """Return the version spec for `name`, or raise KeyError with a helpful list."""
    try:
        return VERSIONS[name]
    except KeyError:
        raise KeyError(
            f"unknown version {name!r} (known: {sorted(VERSIONS)})") from None
